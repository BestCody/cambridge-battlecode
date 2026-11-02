"""Champions carrying a bag: longest alive stretches with a constant gem count > 0.

    python bags.py rounds.txt [min_rounds=60]
"""
import re
import sys
from collections import defaultdict

rows = open(sys.argv[1], encoding="utf-8").read().splitlines()
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60
seen = set()
hist = defaultdict(list)  # champ -> [(round, x, y, hp, gems)]
for line in rows:
    m = re.match(r"r(\d+) ", line)
    if not m:
        continue
    r = int(m.group(1))
    if r in seen:
        continue
    seen.add(r)
    for part in line.split(" | ")[1:]:
        m = re.match(r"(\w+)=(-?\d+),(-?\d+) hp(\d+) g(-?\d+)", part)
        if not m:
            continue
        name, x, y, hp, g = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
        hist[name].append((r, x, y, hp, g))
for name, h in sorted(hist.items()):
    start = None
    for i, (r, x, y, hp, g) in enumerate(h + [(-1, 0, 0, 0, -1)]):
        if start is not None and (g != h[start][4] or hp <= 0):
            n = r - h[start][0] if r >= 0 else h[i - 1][0] - h[start][0]
            if n >= limit:
                s = h[start]
                e = h[i - 1]
                print("%-10s gems %3d for %3d rounds r%d-%d  from (%d,%d) hp%d  to (%d,%d) hp%d  then %s" % (
                    name, s[4], n, s[0], e[0], s[1], s[2], s[3], e[1], e[2], e[3],
                    "dies" if hp <= 0 else "gems %d" % g))
            start = None
        if start is None and g > 0 and hp > 0:
            start = i
