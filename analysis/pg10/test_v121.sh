#!/bin/bash
# v121 = v119 + health builds (repeats alternate with Bruiser/Health, Ryth's repeat Bruiser/Health): the wide block for v119 (live) and v121 in the same hour, then re-pin v119
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
mk v119w analysis/pg10/opponents_wide.txt
cambc submit bots/baseline -n "v121 = v119 + health builds: repeats alternate with Bruiser/Health" </dev/null 2>&1 | tail -1
waitlive 121 || exit 1
mk v121w analysis/pg10/opponents_wide.txt
cambc submissions activate v119 </dev/null 2>&1 | tail -1; echo "repinned v119 at $(date +%T)"
for b in v119w v121w; do bash analysis/pg10/results.sh analysis/pg10/groups_$b.txt analysis/pg10/results_$b.txt; done
