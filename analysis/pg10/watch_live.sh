#!/bin/bash
# usage: watch_live.sh VERSION TAG OPPFILE LADDER -> wait for VERSION live, create groups, re-pin LADDER
want=$1; tag=$2; opp=$3; ladder=$4
for i in $(seq 1 90); do
  live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*")
  if [ "$live" = "$want" ]; then echo "live $want at $(date +%T)"; break; fi
  sleep 10
done
[ "$live" = "$want" ] || { echo "never live: $live"; exit 1; }
out=analysis/pg10/groups_$tag.txt; : > $out
while read name id maps sides; do
  g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null)
  echo "$name $g" >> $out
done < $opp
echo "groups made at $(date +%T)"; cat $out
cambc submissions activate $ladder </dev/null 2>&1 | tail -2
echo "repinned $ladder at $(date +%T)"
