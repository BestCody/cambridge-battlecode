import sys, os, glob, subprocess, statistics
sys.path.insert(0, 'scripts/analysis')
from qparse import load, d
# usage: packshare.py OUT_DIR CONFIG... -> per config over our champion-rounds 30-1200: median nearest-teammate distance, alone (>4) share, same with an enemy within 8
out_dir = sys.argv[1]; cfgs = sys.argv[2:]
cache = os.path.join(out_dir, 'rounds_cache'); os.makedirs(cache, exist_ok=True)
for cfg in cfgs:
    near = []; alone = 0; n = 0; we = []; alone_e = 0; ne = 0
    for rep in sorted(glob.glob(f'{out_dir}/{cfg}-*.repl26a')):
        base = os.path.basename(rep)[:-8]; seat = [x for x in base.split('-') if x in ('A', 'B')][0]
        rt = os.path.join(cache, base + '.rounds.txt')
        if not os.path.exists(rt) or os.path.getsize(rt) == 0:
            with open(rt, 'w') as f: subprocess.run(['node', 'rounds.mjs', os.path.abspath(rep)], cwd='scripts/analysis', stdout=f, stderr=subprocess.DEVNULL)
        for r, _, st in load(rt):
            if r < 30 or r > 1200: continue
            ours = [(k, v) for k, v in st.items() if v and k.endswith(seat) and v['hp'] > 0]
            foes = [v for k, v in st.items() if v and not k.endswith(seat) and v['hp'] > 0]
            for k, v in ours:
                mates = [d(v, t) for k2, t in ours if k2 != k]
                if not mates: continue
                m = min(mates); near.append(m); n += 1; alone += m > 4
                if any(d(v, f) <= 8 for f in foes): we.append(m); ne += 1; alone_e += m > 4
    print(f"{cfg:6} champion-rounds {n:>6}: nearest teammate median {statistics.median(near) if near else 0:.0f}, alone {alone/max(1,n):.0%} | with an enemy within 8: {ne:>5}, median {statistics.median(we) if we else 0:.0f}, alone {alone_e/max(1,ne):.0%}")
