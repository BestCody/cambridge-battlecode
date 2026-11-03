import sys, os, json, glob, collections, math, subprocess
# usage: cyclestat.py OUT_DIR CONFIG... -> per config over an evaluate output dir: our clears, returns to one of our last three
# groups, bank trips and mean bag, gem losses, deaths; layouts are built once per replay into OUT_DIR/layout_cache
out = sys.argv[1]; cfgs = sys.argv[2:]
cache = os.path.join(out, 'layout_cache'); os.makedirs(cache, exist_ok=True)
def layout_of(rep):
    base = os.path.basename(rep)[:-8]
    lp = os.path.join(cache, base + '.layout.txt')
    if not os.path.exists(lp) or os.path.getsize(lp) == 0:
        with open(lp, 'w') as f:
            subprocess.run(['node', 'layout.mjs', os.path.abspath(rep)], cwd='scripts/analysis', stdout=f, stderr=subprocess.DEVNULL)
    lay = {}
    for l in open(lp, encoding='utf-8', errors='replace'):
        if l.startswith('group'):
            t = l.split(); g = int(t[1]); gems = int(t[2].split('=')[1])
            pts = [tuple(int(v) for v in x.split('@')[1].split(',')) for x in t[5:] if '@' in x]
            if pts: lay[g] = (gems, sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))
    return lay
for cfg in cfgs:
    agg = collections.Counter(); bags = []
    for rep in sorted(glob.glob(f'{out}/{cfg}-*.repl26a')):
        base = os.path.basename(rep)[:-8]; seat = [x for x in base.split('-') if x in ('A', 'B')][0]; side = 'team' + seat
        summ = json.load(open(rep[:-8] + '.json', encoding='utf-8')); lay = layout_of(rep)
        seq = collections.defaultdict(list)
        for ev in summ['events']:
            k = ev['kind']; team = ev['owner']['team']
            if team != side: continue
            for en in ev['entries']:
                if k == 'campClear' and en['target']['id'] in lay: seq[ev['owner']['name']].append((en['timestamp']['round'], en['target']['id']))
                elif k == 'gemSubmission':
                    v = float(en.get('value') or 0)
                    if v > 0: agg['banks'] += 1; bags.append(v)
                elif k == 'gemLoss': agg['lost'] += float(en.get('value') or 0)
                elif k == 'death' and ev['owner']['kind'] == 'champion': agg['deaths'] += 1
        for champ, lst in seq.items():
            lst.sort(); recent = []
            for r, g in lst:
                agg['clears'] += 1
                if recent: agg['hops'] += 1; agg['repeat'] += g in recent
                recent = (recent + [g])[-3:]
        agg['games'] += 1; agg['gems'] += int([t['submittedGems'] for t in summ['teams'] if t['team'] == side][0])
    n = max(1, agg['games'])
    print(f"{cfg:8} games {agg['games']:3}: clears/game {agg['clears']/n:5.1f}  returns to a recent group {agg['repeat']/max(1,agg['hops']):4.0%}  banks/game {agg['banks']/n:5.1f}  mean bag {sum(bags)/max(1,len(bags)):5.1f}  lost/game {agg['lost']/n:5.0f}  deaths/game {agg['deaths']/n:4.1f}  banked/game {agg['gems']/n:5.0f}")
