import sys, os, json, glob, collections, re, subprocess
# usage: nearclear.py OUT_DIR CONFIG... -> per config over an evaluate output dir: our champions within 6 of a group when we clear it
out = sys.argv[1]; cfgs = sys.argv[2:]
cache = os.path.join(out, 'rounds_cache'); os.makedirs(cache, exist_ok=True)
lcache = os.path.join(out, 'layout_cache'); os.makedirs(lcache, exist_ok=True)
def rounds_of(rep):
    base = os.path.basename(rep)[:-8]; rt = os.path.join(cache, base + '.rounds.txt')
    if not os.path.exists(rt) or os.path.getsize(rt) == 0:
        with open(rt, 'w') as f: subprocess.run(['node', 'rounds.mjs', os.path.abspath(rep)], cwd='scripts/analysis', stdout=f, stderr=subprocess.DEVNULL)
    rows = {}
    for line in open(rt, encoding='utf-8', errors='replace'):
        r = re.match(r'r(\d+)', line)
        if not r: continue
        st = {}
        for champ in line.split('|')[1:]:
            champ = champ.strip()
            if '=' not in champ: continue
            name, rest = champ.split('=', 1)
            try: x, y = (int(v) for v in rest.split()[0].split(','))
            except Exception: continue
            hp = re.search(r'hp(\d+)', champ)
            if hp and int(hp.group(1)) > 0: st[name] = (x, y)
        rows[int(r.group(1))] = st
    return rows
def lay_of(rep):
    base = os.path.basename(rep)[:-8]; lp = os.path.join(lcache, base + '.layout.txt')
    if not os.path.exists(lp) or os.path.getsize(lp) == 0:
        with open(lp, 'w') as f: subprocess.run(['node', 'layout.mjs', os.path.abspath(rep)], cwd='scripts/analysis', stdout=f, stderr=subprocess.DEVNULL)
    lay = {}
    for l in open(lp, encoding='utf-8', errors='replace'):
        if l.startswith('group'):
            t = l.split(); g = int(t[1]); gems = int(t[2].split('=')[1])
            pts = [tuple(int(v) for v in x.split('@')[1].split(',')) for x in t[5:] if '@' in x]
            if pts: lay[g] = (gems, sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))
    return lay
for cfg in cfgs:
    agg = collections.Counter()
    for rep in sorted(glob.glob(f'{out}/{cfg}-*.repl26a')):
        base = os.path.basename(rep)[:-8]; seat = [x for x in base.split('-') if x in ('A', 'B')][0]
        rows = rounds_of(rep); lay = lay_of(rep); s = json.load(open(rep[:-8] + '.json', encoding='utf-8'))
        for ev in s['events']:
            if ev['kind'] != 'campClear' or ev['owner']['team'] != 'team' + seat: continue
            for en in ev['entries']:
                g = en['target']['id']; r = en['timestamp']['round']
                if g not in lay or r < 30: continue
                st = rows.get(r - 1, {}); gx, gy = lay[g][1], lay[g][2]
                near = sum(1 for k, (x, y) in st.items() if k.endswith(seat) and max(abs(x - gx), abs(y - gy)) <= 6)
                agg['n'] += 1; agg['near'] += near; agg['solo'] += near <= 1; agg['three'] += near >= 3
                if lay[g][0] >= 25: agg['bn'] += 1; agg['bnear'] += near
    print(f"{cfg:8} clears {agg['n']:5}: near at the clear {agg['near']/max(1,agg['n']):4.2f}  solo {agg['solo']/max(1,agg['n']):4.0%}  three+ {agg['three']/max(1,agg['n']):4.0%}  on 25+ gem groups {agg['bnear']/max(1,agg['bn']):4.2f}")
