#!/bin/bash
# v120 = v119 + chains of dashes over stepping stones (the pond's island, islands' elk groups): the pocket block against v119p/v112p of the same hour, then re-pin v119
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
cambc submit bots/baseline -n "v120 = v119 + chains of dashes over stepping stones (the pond's island)" </dev/null 2>&1 | tail -1
waitlive 120 || exit 1
mk v120p analysis/pg10/opponents_pocket.txt
cambc submissions activate v119 </dev/null 2>&1 | tail -1; echo "repinned v119 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v120p.txt analysis/pg10/results_v120p.txt
