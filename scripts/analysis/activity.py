"""How each champion spent its rounds: dead, idle (no move or action), walking only, acting (attack or ability).

    python activity.py rounds.txt [FROM TO]
"""
import re
import sys
from collections import defaultdict, Counter

rows = open(sys.argv[1], encoding="utf-8").read().splitlines()
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
seen = set()
tally = defaultdict(Counter)
for line in rows:
    m = re.match(r"r(\d+) ", line)
    if not m:
        continue
    r = int(m.group(1))
    if r in seen or r < lo or r > hi:
        continue
    seen.add(r)
    for part in line.split(" | ")[1:]:
        m = re.match(r"(\w+)=(-?\d+),(-?\d+) hp(\d+) g(-?\d+) s(\w+)(.*)", part)
        if not m:
            continue
        name, hp, rest = m.group(1), int(m.group(4)), m.group(7)
        if hp <= 0:
            kind = "dead"
        elif "atk>" in rest or "abl" in rest:
            kind = "acting"
        elif "mv>" in rest:
            kind = "walking"
        else:
            kind = "idle"
        tally[name][kind] += 1
print("%-12s %5s %5s %5s %5s" % ("champ", "dead", "idle", "walk", "act"))
for name in sorted(tally, key=lambda n: (n[-1], n)):
    t = tally[name]
    print("%-12s %5d %5d %5d %5d" % (name, t["dead"], t["idle"], t["walking"], t["acting"]))
