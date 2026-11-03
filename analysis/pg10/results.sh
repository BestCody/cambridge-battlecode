#!/bin/bash
# usage: results.sh GROUPS_FILE OUT -> waits for every group in GROUPS_FILE (name id), writes per-map results to OUT
gf=$1; out=$2; : > $out
for i in $(seq 1 60); do
  : > $out.tmp; done_all=1
  while read name gid; do
    cambc match-groups show $gid --json </dev/null 2>/dev/null | python -I -c "
import sys,json
d=json.load(sys.stdin); ms=d['matches']; w=0
for m in ms:
    us=[t for t in m['teams'] if t['teamId']=='c2892f58-a3ad-4ff7-a256-120445ad6ee7'][0]
    print('  %-22s %-9s %-5s %s' % (m['map'], m['status'], us.get('result'), m['matchId']))
    w += us.get('result')=='win'
print('%s %s %d/%d %s' % ('$gid'[:8], '$name', w, len(ms), 'DONE' if all(m['status']=='finished' for m in ms) else 'WAIT'))" >> $out.tmp
  done < $gf
  grep -q WAIT $out.tmp || { mv $out.tmp $out; cat $out; exit 0; }
  sleep 10
done
mv $out.tmp $out; cat $out
