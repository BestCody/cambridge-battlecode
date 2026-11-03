#!/bin/bash
# v123 = v119 + bank_min_bag 40 (the voluntary bank trips carry 40 gems or more): the wide block for v119 (live) and v123, ten groups each (the quota is 50 matches an hour, so the second block waits for it), then re-pin v119
mk() { out=analysis/pg10/groups_$1.txt; : > $out; n=0; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); [ -n "$g" ] && n=$((n+1)); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date -u +%H:%MZ): $n created"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date -u +%H:%MZ)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
waitquota() { for t in $(seq 1 70); do err=$(cambc match-groups create 626450a6-dab1-46a3-b4f4-0df7867e275f --maps "00vault,01big_world,03membrane,00gemfight,02the_pond" --sides a,b,a,b,a --json </dev/null 2>&1); if echo "$err" | grep -q groupId; then echo "$err" | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])"; return 0; fi; sleep 60; done; return 1; }
g=$(waitquota) || { echo "quota never freed for the reference"; exit 1; }
out=analysis/pg10/groups_v119w6.txt; : > $out; echo "DING $g" >> $out
while read name id maps sides; do [ "$name" = "DING" ] && continue; g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < analysis/pg10/opponents_wide.txt
echo "groups v119w6 at $(date -u +%H:%MZ)"; cat $out
cambc submit bots/baseline -n "v125 = v119 + join_share 0: joining a claimed group is worth the group (packs)" </dev/null 2>&1 | tail -1
waitlive 125 || exit 1
g=$(waitquota) || { echo "quota never freed"; cambc submissions activate v119 </dev/null 2>&1 | tail -1; exit 1; }
out=analysis/pg10/groups_v125w.txt; : > $out; echo "DING $g" >> $out
while read name id maps sides; do [ "$name" = "DING" ] && continue; g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < analysis/pg10/opponents_wide.txt
echo "groups v125w at $(date -u +%H:%MZ)"; cat $out
cambc submissions activate v119 </dev/null 2>&1 | tail -1; echo "repinned v119 at $(date -u +%H:%MZ)"
for b in v119w6 v125w; do bash analysis/pg10/results.sh analysis/pg10/groups_$b.txt analysis/pg10/results_$b.txt; done
