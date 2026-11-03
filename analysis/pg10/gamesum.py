import sys, os, json, collections, subprocess
import sys as _s; _s.stdout.reconfigure(encoding="utf-8", errors="replace")
# usage: gamesum.py DIR... -> teams (from match.json, fetched once), result, gems banked at 300/600/900/1200/1500/2000, deaths per side
def rle(runs):
    out = []
    for r in runs:
        out.extend([r['value']] * int(r['duration']))
    return out
for d in sys.argv[1:]:
    p = os.path.join(d, 'summary.json')
    if not os.path.exists(p): continue
    s = json.load(open(p, encoding='utf-8'))
    mj = os.path.join(d, 'match.json')
    names = {}
    mid = None
    if os.path.exists(mj):
        m = json.load(open(mj, encoding='utf-8'))
    else:
        m = None
        # the match id is the zip's name in the fetch, else unknown
        for f in os.listdir(d):
            if f.endswith('.repl26a') and len(f) > 30: mid = f[:-8]
    if m:
        for t in m['matches'][0]['teams']:
            names['team' + t['side'].upper()] = (t['name'], t['result'])
    deaths = collections.Counter()
    for ev in s.get('events', []):
        if ev.get('kind') == 'death' and ev.get('owner', {}).get('kind') == 'champion':
            deaths[ev['owner']['team']] += len(ev.get('entries', []))
    gems = {}
    for tl in s.get('teamTimelines', []):
        if tl.get('metric') == 'submittedGems':
            gems[tl['owner']] = rle(tl['runs'])
    line = f"{os.path.basename(d.rstrip('/')):36}"
    for side in ('teamA', 'teamB'):
        g = gems.get(side, [])
        nm, res = names.get(side, ('?', '?'))
        at = '/'.join(str(g[min(r, len(g) - 1)]) if g else '?' for r in (300, 600, 900, 1200, 1500, 2000))
        line += f" | {side[-1]} {nm:22} {res:4} d{deaths.get(side,0):<3} {at}"
    print(line)
