import json, sys, collections
# usage: early.py DIR SIDE UNTIL -> captures by group (gold, gems) per 100 rounds for SIDE, with gold per champion at 100/200/300
d = json.load(open(sys.argv[1] + '/summary.json', encoding='utf-8')); side = 'team' + sys.argv[2]; until = int(sys.argv[3])
lay = {}
for l in open(sys.argv[1] + '/layout.txt'):
    if l.startswith('group'):
        p = l.split(); lay[int(p[1])] = (int(p[2].split('=')[1]), int(p[3].split('=')[1]), ' '.join(x.split('@')[0] for x in p[5:])[:40])
caps = collections.defaultdict(list)
for e in d['events']:
    if e['kind'] == 'gemCollection' and e['owner'].get('team') == side:
        for en in e['entries']:
            r = en['timestamp']['round']
            if r <= until: caps[r // 100].append((r, en['target']['id'], int(float(en['value']))))
names = {c['id']: c['name'][:7] for c in d['roster']['champions'] if c['team'] == side}
tl = {}
for e in d['championTimelines']:
    o = e['owner']
    if o['team'] == side: tl[(o['id'], e['metric'])] = e['runs']
def at(runs, rnd):
    r = 0; v = 0
    for x in runs:
        if r + x['duration'] > rnd: return v
        r += x['duration']; v = float(x['value'])
    return v
for b in sorted(caps):
    items = caps[b]
    gold = sum(lay.get(g, (0, 0))[1] for _, g, _ in items); gems = sum(v for _, _, v in items)
    print(f"r{b*100}-{b*100+99}: {len(items)} captures, gold {gold}, gems {gems}: " + ', '.join(f"g{g}({lay.get(g, ('?','?',''))[1]}g/{lay.get(g, ('?','?',''))[0]}gem {lay.get(g, ('','',''))[2][:14]})" for _, g, _ in items)[:300])
for rnd in (100, 200, 300, 500):
    print(f"  gold at r{rnd}:", {names[i]: int(at(tl.get((i, 'earnedGold'), []), rnd)) for i in names}, 'kills', int(sum(at(tl.get((i, 'kills'), []), rnd) for i in names)), 'deaths', int(sum(at(tl.get((i, 'deaths'), []), rnd) for i in names)))
