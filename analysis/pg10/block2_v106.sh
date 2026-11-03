#!/bin/bash
while [ "$(date +%H%M%S)" \< "152030" ]; do sleep 5; done
cambc submissions activate v106 </dev/null 2>&1 | tail -1; echo "selected 106 at $(date +%T)"
for i in $(seq 1 60); do
  live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*")
  [ "$live" = "106" ] && { echo "live 106 at $(date +%T)"; break; }; sleep 10
done
[ "$live" = "106" ] || { echo "never live: $live"; cambc submissions activate v79 </dev/null >/dev/null 2>&1; exit 1; }
out=analysis/pg10/groups_v106b.txt; : > $out
while read name id maps sides; do
  g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null)
  echo "$name $g" >> $out
done < analysis/pg10/opponents_v106b.txt
echo "groups made at $(date +%T)"; cat $out
cambc submissions activate v79 </dev/null 2>&1 | tail -1; echo "repinned v79 at $(date +%T)"
