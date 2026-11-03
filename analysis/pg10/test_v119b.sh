#!/bin/bash
# v119 continued: the v112p reference groups exist (groups_v112p.txt); submit v119, its groups on the same block, re-pin v112, results
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
cambc submit bots/baseline -n "v119 = v112 + pocket entries: dash into hole-ringed gem groups (big_world)" </dev/null 2>&1 | tail -1
waitlive 119 || exit 1
mk v119p analysis/pg10/opponents_pocket.txt
cambc submissions activate v112 </dev/null 2>&1 | tail -1; echo "repinned v112 at $(date +%T)"
for b in v112p v119p; do bash analysis/pg10/results.sh analysis/pg10/groups_$b.txt analysis/pg10/results_$b.txt; done
