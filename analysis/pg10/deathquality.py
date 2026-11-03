import sys, os, json, glob, re, subprocess, collections
sys.path.insert(0, 'scripts/analysis')
from qparse import load, d
# usage: deathquality.py OUT_DIR CONFIG... -> per config, over our deaths in every replay: share from above 60% of max health
# eight rounds before, share killed within 3 rounds from above 80% of that, share alone (nearest teammate > 4), share with 2+ foes within 4
out_dir = sys.argv[1]; cfgs = sys.argv[2:]
cache = os.path.join(out_dir, 'rounds_cache'); os.makedirs(cache, exist_ok=True)
for cfg in cfgs:
    agg = collections.Counter(); n = 0
    for rep in sorted(glob.glob(f'{out_dir}/{cfg}-*.repl26a')):
        base = os.path.basename(rep)[:-8]; seat = [x for x in base.split('-') if x in ('A', 'B')][0]
        rt = os.path.join(cache, base + '.rounds.txt')
        if not os.path.exists(rt) or os.path.getsize(rt) == 0:
            with open(rt, 'w') as f: subprocess.run(['node', 'rounds.mjs', os.path.abspath(rep)], cwd='scripts/analysis', stdout=f, stderr=subprocess.DEVNULL)
        rows = {r: st for r, _, st in load(rt)}
        summ = json.load(open(rep[:-8] + '.json', encoding='utf-8'))
        maxhp = {}
        for r, st in rows.items():
            for k, v in st.items():
                if v: maxhp[k] = max(maxhp.get(k, 0), v['hp'])
        for e in summ['events']:
            if e['kind'] != 'death' or e['owner']['team'] != 'team' + seat: continue
            for en in e['entries']:
                r = en['timestamp']['round']; name = e['owner']['name'].replace(' ', '')
                key = None
                for k in rows.get(r - 1, {}):
                    if k.endswith(seat) and (k[:-1] == name or (name.startswith('Heart') and k[:-1] == 'HoR') or k[:-1].startswith(name.split()[0][:5])): key = k; break
                if not key: continue
                v1 = rows.get(r - 1, {}).get(key); v3 = rows.get(r - 3, {}).get(key); v8 = rows.get(r - 8, {}).get(key)
                if not v1 or not v8: continue
                mh = maxhp.get(key, 1000) or 1000
                n += 1
                if v8['hp'] >= 0.6 * mh: agg['from_above_60'] += 1
                if v3 and v3['hp'] >= 0.8 * v8['hp']: agg['burst_3_rounds'] += 1
                st = rows.get(r - 1, {})
                mates = [d(v1, t) for k2, t in st.items() if t and k2 != key and k2.endswith(seat) and t['hp'] > 0]
                foes = [d(v1, t) for k2, t in st.items() if t and not k2.endswith(seat) and t['hp'] > 0]
                if not mates or min(mates) > 4: agg['alone'] += 1
                if sum(1 for x in foes if x <= 4) >= 2: agg['two_plus_foes'] += 1
                if not foes or min(foes) > 4: agg['no_foe_near'] += 1
    print(f"{cfg:6} deaths {n:>4}: from above 60% health {agg['from_above_60']/max(1,n):.0%}, burst in 3 rounds {agg['burst_3_rounds']/max(1,n):.0%}, alone {agg['alone']/max(1,n):.0%}, 2+ foes within 4 {agg['two_plus_foes']/max(1,n):.0%}, no foe within 4 {agg['no_foe_near']/max(1,n):.0%}")
