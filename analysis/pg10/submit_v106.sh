#!/bin/bash
# wait for 15:10:30, submit, wait live, wait for the 15:20:45 quota reset, create 6 groups, re-pin v79
while [ "$(date +%H%M%S)" \< "151030" ]; do sleep 5; done
echo "submit at $(date +%T)"
cambc submit bots/baseline -n "v106 = v104 + Prospector then Capture first, early gold 0.35" </dev/null 2>&1 | tail -2
for i in $(seq 1 120); do
  live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*")
  [ "$live" = "106" ] && { echo "live 106 at $(date +%T)"; break; }
  sleep 10
done
[ "$live" = "106" ] || { echo "never live: $live"; exit 1; }
while [ "$(date +%H%M%S)" \< "152100" ]; do sleep 5; done
out=analysis/pg10/groups_v106a.txt; : > $out
while read name id maps sides; do
  g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null)
  echo "$name $g" >> $out
done < analysis/pg10/opponents_v105.txt
echo "groups made at $(date +%T)"; cat $out
cambc submissions activate v79 </dev/null 2>&1 | tail -1
echo "repinned v79 at $(date +%T)"
