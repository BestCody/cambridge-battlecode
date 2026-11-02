import sys, re, statistics
sys.path.insert(0, '.')
from qparse import load, d
rows = load(sys.argv[1])
# For every round where a champion attacks an enemy champion, record the attacker's distance to its nearest living teammate
def att_targets(a):
    return [(int(m[1]), int(m[2])) for m in re.finditer(r"(?:atk|abl\d)>(-?\d+),(-?\d+)", a)]
out = {'A': [], 'B': []}; alone = {'A': 0, 'B': 0}; n = {'A': 0, 'B': 0}
for rnd, sub, st in rows:
    for name, s in st.items():
        if not s or s['hp'] <= 0: continue
        side = name[-1]
        foes = [(t['x'], t['y']) for o, t in st.items() if t and o[-1] != side and t['hp'] > 0]
        if not any(tg in foes for tg in att_targets(s['act'])): continue
        mates = [d(s, t) for o, t in st.items() if t and o != name and o[-1] == side and t['hp'] > 0]
        if not mates: continue
        nearest = min(mates); out[side].append(nearest); n[side] += 1
        if nearest > 4: alone[side] += 1
for side in 'AB':
    v = out[side]
    if v: print(f"side {side}: {len(v)} attacking turns, nearest teammate median {statistics.median(v):.0f}, mean {statistics.mean(v):.1f}, share with no teammate within 4: {alone[side]/len(v):.0%}")
