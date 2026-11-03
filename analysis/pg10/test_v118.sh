#!/bin/bash
# v118 = v112 + the siege calibration (shield 3 saves, heal 2): the fortress and monolith blocks twice, then re-pin v112
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
cambc submit bots/baseline -n "v118 = v112 + siege shield and heal saves from the replays (3 and 2 hits in 8)" </dev/null 2>&1 | tail -1
waitlive 118 || exit 1
mk v118f analysis/pg10/opponents_fort.txt
mk v118s analysis/pg10/opponents_siege.txt
mk v118f2 analysis/pg10/opponents_fort.txt
mk v118s2 analysis/pg10/opponents_siege.txt
cambc submissions activate v112 </dev/null 2>&1 | tail -1; echo "repinned v112 at $(date +%T)"
for b in v118f v118s v118f2 v118s2; do bash analysis/pg10/results.sh analysis/pg10/groups_$b.txt analysis/pg10/results_$b.txt; done
