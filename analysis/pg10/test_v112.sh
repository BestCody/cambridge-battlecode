#!/bin/bash
# submit v112 (v111 + spawn-strike and the fortress lock-in on fortress maps), test it on the monolith and fortress blocks, re-pin v110
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
cambc submit bots/baseline -n "v112 = v111 + fortress at spawn struck from spawn, whole team from round 1" </dev/null 2>&1 | tail -1
waitlive 112 || exit 1
while [ "$(date +%H%M%S)" \< "232215" ]; do sleep 5; done
mk v112s analysis/pg10/opponents_siege.txt
mk v112f analysis/pg10/opponents_fort.txt
cambc submissions activate v110 </dev/null 2>&1 | tail -1; echo "repinned v110 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v112s.txt analysis/pg10/results_v112s.txt
bash analysis/pg10/results.sh analysis/pg10/groups_v112f.txt analysis/pg10/results_v112f.txt
