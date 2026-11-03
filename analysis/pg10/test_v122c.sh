#!/bin/bash
# v122 second block, continued: the quota is 50 unranked matches an hour (ten groups). Activate v122, wait for the quota, create v122w2, re-pin v119, results.
mk() { out=analysis/pg10/groups_$1.txt; : > $out; n=0; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); [ -n "$g" ] && n=$((n+1)); echo "$name $g" >> $out; done < $2; echo "groups $1 at $(date +%T): $n created"; cat $out; return $(( n < 10 )); }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
cambc submissions activate v122 </dev/null 2>&1 | tail -1
waitlive 122 || exit 1
for t in $(seq 1 30); do
  err=$(cambc match-groups create 626450a6-dab1-46a3-b4f4-0df7867e275f --maps "00vault,01big_world,03membrane,00gemfight,02the_pond" --sides a,b,a,b,a --json </dev/null 2>&1)
  if echo "$err" | grep -q groupId; then
    g=$(echo "$err" | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])")
    : > analysis/pg10/groups_v122w2.txt; echo "DING $g" >> analysis/pg10/groups_v122w2.txt
    while read name id maps sides; do [ "$name" = "DING" ] && continue; g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> analysis/pg10/groups_v122w2.txt; done < analysis/pg10/opponents_wide.txt
    echo "groups v122w2 at $(date +%T)"; cat analysis/pg10/groups_v122w2.txt
    break
  fi
  echo "quota wait $t: $(echo "$err" | grep -o 'retry after [0-9]* seconds')"; sleep 60
done
cambc submissions activate v119 </dev/null 2>&1 | tail -1; echo "repinned v119 at $(date +%T)"
bash analysis/pg10/results.sh analysis/pg10/groups_v122w2.txt analysis/pg10/results_v122w2.txt
