import sys, os, json, collections
# usage: goldsrc.py DIR... -> per side: camp-clear gold and kills per 300 rounds, earnedGold at 300/600/900/1200/2000, gems banked
def rle(runs):
    out = []
    for r in runs: out.extend([float(r['value'])] * int(r['duration']))
    return out
for d in sys.argv[1:]:
    p = os.path.join(d, 'summary.json')
    if not os.path.exists(p): continue
    s = json.load(open(p, encoding='utf-8'))
    gold_of = {}
    for l in open(os.path.join(d, 'layout.txt')):
        if l.startswith('group'):
            t = l.split(); gold_of[int(t[1])] = (int(t[2].split('=')[1]), int(t[3].split('=')[1]))
    names = {}
    mj = os.path.join(d, 'match.json')
    if os.path.exists(mj):
        for t in json.load(open(mj, encoding='utf-8'))['matches'][0]['teams']: names['team' + t['side'].upper()] = t['name'][:14] + ' ' + t['result']
    clears = collections.defaultdict(lambda: collections.Counter()); kills = collections.defaultdict(lambda: collections.Counter())
    gemclears = collections.defaultdict(lambda: collections.Counter())
    for ev in s['events']:
        if ev['kind'] == 'campClear':
            team = ev['owner']['team']
            for en in ev['entries']:
                gid = en['target']['id']; b = en['timestamp']['round'] // 300
                clears[team][b] += gold_of.get(gid, (0, 0))[1]; gemclears[team][b] += gold_of.get(gid, (0, 0))[0]
        if ev['kind'] == 'kill':
            team = ev['owner']['team']
            for en in ev['entries']: kills[team][en['timestamp']['round'] // 300] += 1
    earned = collections.defaultdict(lambda: [0.0] * 2001)
    for tl in s['championTimelines']:
        if tl['metric'] == 'earnedGold':
            v = rle(tl['runs']); team = tl['owner']['team']
            e = earned[team]
            for i in range(min(len(v), 2001)): e[i] += v[i]
    print('--', os.path.basename(d.rstrip('/')))
    for team in ('teamA', 'teamB'):
        e = earned[team]
        print(f"  {team} {names.get(team,'?'):20} clear-gold/300r: {[clears[team][b] for b in range(7)]} gem-clears/300r: {[gemclears[team][b] for b in range(7)]} kills/300r: {[kills[team][b] for b in range(7)]} earned at 300/600/900/1200/2000: {[int(e[min(r,len(e)-1)]) for r in (300,600,900,1200,2000)]}")
