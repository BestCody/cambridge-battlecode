#!/bin/bash
# after the v112 shared-set reference (results_v112a.txt): submit v117 (v112 + repeats alternating Bruiser/Health, Ryth's repeat Health),
# run the shared set, the monolith block and the fortress block (14 groups, 70 games), re-pin v112
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
for i in $(seq 1 120); do [ -s analysis/pg10/results_v112a.txt ] && break; sleep 10; done
[ -s analysis/pg10/results_v112a.txt ] || { echo "no v112a results"; exit 1; }
cambc submit bots/baseline -n "v117 = v112 + repeats alternate with Bruiser/Health, Ryth repeats Health" </dev/null 2>&1 | tail -1
waitlive 117 || exit 1
mk v117a analysis/pg10/opponents_v105.txt
mk v117s analysis/pg10/opponents_siege.txt
mk v117f analysis/pg10/opponents_fort.txt
cambc submissions activate v112 </dev/null 2>&1 | tail -1; echo "repinned v112 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v117a.txt analysis/pg10/results_v117a.txt
bash analysis/pg10/results.sh analysis/pg10/groups_v117s.txt analysis/pg10/results_v117s.txt
bash analysis/pg10/results.sh analysis/pg10/groups_v117f.txt analysis/pg10/results_v117f.txt
