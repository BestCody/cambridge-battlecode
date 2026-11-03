import sys, re, json
sys.path.insert(0, '.')
from qparse import load, d
# usage: deathctx.py DIR SIDE  -> for each death of SIDE: round, victim, hp 8 rounds before, nearest mate, mates/foes within 4, attackers in the last 3 rounds
dirn, side = sys.argv[1], sys.argv[2]
rows = load(dirn + '/rounds.txt'); byr = {r: st for r, _, st in rows}
summ = json.load(open(dirn + '/summary.json', encoding='utf-8'))
deaths = []
for e in summ['events']:
    if e['kind'] != 'death' or e['owner']['team'] != 'team' + side: continue
    for en in e['entries']: deaths.append((en['timestamp']['round'], e['owner']['name'], en['x'], en['y']))
deaths.sort()
def att_targets(a): return [(int(m[1]), int(m[2])) for m in re.finditer(r"(?:atk|abl\d)>(-?\d+),(-?\d+)", a)]
print(f"{'round':>5} {'victim':14} {'hp-8':>5} {'hp-3':>5} {'mate':>4} {'m4':>2} {'f4':>2}  attackers (last 3 rounds)")
for r, name, x, y in deaths:
    key = name.replace(' ', '')[:8]
    def find(st):
        for k, v in st.items():
            if v and k.endswith(side) and (name.replace(' ', '') == k[:-1] or k[:-1].startswith(name.split()[0])): return k, v
        return None, None
    st = byr.get(r - 1, {}); k, v = find(st)
    if not v: print(r, name, 'no state'); continue
    hp8 = find(byr.get(r - 8, {}))[1]; hp3 = find(byr.get(r - 3, {}))[1]
    mates = [d(v, t) for o, t in st.items() if t and o != k and o[-1] == side and t['hp'] > 0]
    foes = [(o, t) for o, t in st.items() if t and o[-1] != side and t['hp'] > 0]
    m4 = sum(1 for m in mates if m <= 4); f4 = sum(1 for o, t in foes if d(v, t) <= 4)
    atk = {}
    for rr in range(r - 3, r + 1):
        s2 = byr.get(rr, {}); kk, vv = find(s2)
        if not vv: continue
        for o, t in s2.items():
            if t and o[-1] != side and (vv['x'], vv['y']) in att_targets(t['act']): atk[o] = atk.get(o, 0) + 1
    print(f"{r:>5} {name:14} {hp8['hp'] if hp8 else '-':>5} {hp3['hp'] if hp3 else '-':>5} {min(mates) if mates else '-':>4} {m4:>2} {f4:>2}  {atk}")
