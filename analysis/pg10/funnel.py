import json, sys, os, collections
# usage: funnel.py SPEC_JSON TEAMNAME -> per game: the team's gold per champion, top share, top champion's emblems and kills
spec = json.load(open(sys.argv[1])); team = sys.argv[2]
rows = []
for s in spec:
    if not os.path.exists(s['file']) or os.path.getsize(s['file']) < 1000: continue
    d = json.load(open(s['file'], encoding='utf-8'))
    us = 'team' + s['side'].upper()
    champs = {c['id']: c for c in d['roster']['champions'] if c['team'] == us}
    tl = {}
    for e in d['championTimelines']:
        o = e['owner']
        if o['team'] == us: tl[(o['id'], e['metric'])] = e['runs']
    def final(cid, metric):
        runs = tl.get((cid, metric)) or []
        return float(runs[-1]['value']) if runs else 0
    gold = {cid: final(cid, 'earnedGold') for cid in champs}
    kills = {cid: final(cid, 'kills') for cid in champs}
    deaths = {cid: final(cid, 'deaths') for cid in champs}
    embl = collections.Counter(); first = {}
    for e in d['events']:
        if e['kind'] == 'itemCombination' and e['owner'].get('team') == us:
            cid = e['owner']['id']
            for en in e['entries']:
                embl[cid] += 1; first.setdefault(cid, en['timestamp']['round'])
    tg = sum(gold.values()) or 1
    top = max(gold, key=gold.get)
    rows.append((s['map'], s['result'], s['opp'][:14], len(champs), tg, champs[top]['name'], gold[top] / tg, embl[top], first.get(top, -1), kills[top], sum(kills.values()), sum(deaths.values()), ' '.join(f"{champs[c]['name'][:3]}:{gold[c]}/{embl[c]}" for c in sorted(gold, key=gold.get, reverse=True))))
print(f"{'map':20} {'res':4} {'opp':14} n  teamgold top            share emb 1st  Ktop K  D   gold/emblems by champion")
for r in rows: print(f"{r[0]:20} {r[1]:4} {r[2]:14} {r[3]}  {r[4]:>6} {r[5]:14} {r[6]:.2f}  {r[7]:>2} {r[8]:>4} {r[9]:>3} {r[10]:>2} {r[11]:>2}   {r[12]}")
import statistics
print('games', len(rows), 'median top share', statistics.median(r[6] for r in rows), 'mean', sum(r[6] for r in rows)/len(rows))
print('top champion by name:', collections.Counter(r[5] for r in rows))
