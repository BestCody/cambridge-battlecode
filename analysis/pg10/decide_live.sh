#!/bin/bash
# the same 50 games (ten opponents, five maps each) on v112 and then v110 in the same hour; re-pin v112 at the end
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
waitlive 112 || exit 1
mk v112w analysis/pg10/opponents_wide.txt
bash analysis/pg10/results.sh analysis/pg10/groups_v112w.txt analysis/pg10/results_v112w.txt
cambc submissions activate v110 </dev/null 2>&1 | tail -1
waitlive 110 || exit 1
mk v110w analysis/pg10/opponents_wide.txt
cambc submissions activate v112 </dev/null 2>&1 | tail -1; echo "repinned v112 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v110w.txt analysis/pg10/results_v110w.txt
