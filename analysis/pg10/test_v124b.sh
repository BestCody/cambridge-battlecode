#!/bin/bash
# v124 on the platform = v119 + join_share 0 (packs at the clear). The v119 reference block (groups_v119w6.txt) exists; activate v124, wait for the quota, create its ten groups, re-pin v119, results.
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date -u +%H:%MZ)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
waitquota() { for t in $(seq 1 75); do err=$(cambc match-groups create 626450a6-dab1-46a3-b4f4-0df7867e275f --maps "00vault,01big_world,03membrane,00gemfight,02the_pond" --sides a,b,a,b,a --json </dev/null 2>&1); if echo "$err" | grep -q groupId; then echo "$err" | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])"; return 0; fi; sleep 60; done; return 1; }
cambc submissions activate v124 </dev/null 2>&1 | tail -1
waitlive 124 || exit 1
g=$(waitquota) || { echo "quota never freed"; cambc submissions activate v119 </dev/null 2>&1 | tail -1; exit 1; }
out=analysis/pg10/groups_v124w.txt; : > $out; echo "DING $g" >> $out
while read name id maps sides; do [ "$name" = "DING" ] && continue; g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < analysis/pg10/opponents_wide.txt
echo "groups v124w at $(date -u +%H:%MZ)"; cat $out
cambc submissions activate v119 </dev/null 2>&1 | tail -1; echo "repinned v119 at $(date -u +%H:%MZ)"
for b in v119w6 v124w; do bash analysis/pg10/results.sh analysis/pg10/groups_$b.txt analysis/pg10/results_$b.txt; done
