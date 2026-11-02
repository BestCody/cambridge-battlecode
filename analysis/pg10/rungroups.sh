#!/bin/bash
# usage: rungroups.sh TAG  -> creates one group per opponent, polls, prints per-opponent totals
tag=$1; out=analysis/pg10/groups_$tag.txt; : > $out
while read name id maps sides; do
  g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null)
  echo "$name $g" >> $out; sleep 2
done < ${2:-analysis/pg10/opponents.txt}
ids=$(awk '{print $2}' $out | tr '\n' ' ')
for i in $(seq 1 40); do o=$(python -I analysis/pg8/show.py $ids 2>&1); echo "$o" | grep -q "running\|queued" || break; sleep 20; done
echo "$o" > analysis/pg10/results_$tag.txt
echo "$o" | grep -v "finished"
