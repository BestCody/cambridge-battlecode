#!/bin/bash
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < analysis/pg10/opponents_v107.txt; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 120); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
while [ "$(date +%H%M%S)" \< "170030" ]; do sleep 5; done
cambc submit bots/baseline -n "v108 = v107 + species fight gate only when alone on anvil and gemfight" </dev/null 2>&1 | tail -1
waitlive 108 && mk v108a
cambc submissions activate v79 </dev/null 2>&1 | tail -1; echo "repinned v79 at $(date +%T)"
