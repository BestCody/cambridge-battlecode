#!/bin/bash
# submit v113 (v112 + the vouch), test it on the six-opponent 30-game shared set (reference: v110a 22/30, v106 20, v79 12), re-pin v112
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
while [ "$(date +%H%M%S)" \< "234840" ]; do sleep 5; done
cambc submit bots/baseline -n "v113 = v112 + the vouch (per-group tally of deaths, abandons, captures)" </dev/null 2>&1 | tail -1
waitlive 113 || exit 1
mk v113a analysis/pg10/opponents_v105.txt
cambc submissions activate v112 </dev/null 2>&1 | tail -1; echo "repinned v112 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v113a.txt analysis/pg10/results_v113a.txt
