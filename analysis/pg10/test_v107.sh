#!/bin/bash
# v106 reference on the routed maps, then submit v107 (routed species) and test it on the same maps; re-pin v79 after.
mk() { out=analysis/pg10/groups_$1.txt; : > $out; while read name id maps sides; do g=$(cambc match-groups create $id --maps "$maps" --sides $sides --json </dev/null 2>&1 | python -I -c "import sys,json; print(json.load(sys.stdin)['groupId'])" 2>/dev/null); echo "$name $g" >> $out; done < analysis/pg10/opponents_v107.txt; echo "groups $1 at $(date +%T)"; cat $out; }
waitlive() { for i in $(seq 1 90); do live=$(cambc submissions list </dev/null 2>&1 | grep -o "Live: [0-9]*" | grep -o "[0-9]*"); [ "$live" = "$1" ] && { echo "live $1 at $(date +%T)"; return 0; }; sleep 10; done; echo "never live $1"; return 1; }
while [ "$(date +%H%M%S)" \< "163030" ]; do sleep 5; done
cambc submissions activate v106 </dev/null >/dev/null 2>&1; echo "selected 106 at $(date +%T)"
waitlive 106 && mk v106r
while [ ! -f analysis/routed_140/games.json ]; do sleep 10; done
[ -f analysis/pg10/STOP_V107 ] && { cambc submissions activate v79 </dev/null >/dev/null 2>&1; echo stopped; exit 0; }
cambc submit bots/baseline -n "v107 = v106 + harvest species routed to 12 maps (no-death farming, cohesion)" </dev/null 2>&1 | tail -1
waitlive 107 && mk v107a
cambc submissions activate v79 </dev/null 2>&1 | tail -1; echo "repinned v79 at $(date +%T)"
