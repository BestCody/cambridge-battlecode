import sys, os, json
# usage: gemflow.py DIR UNTIL -> chronological gem events (collection, loss, submission, death, campClear of big groups) for both teams
d = sys.argv[1]; until = int(sys.argv[2])
s = json.load(open(os.path.join(d, 'summary.json'), encoding='utf-8'))
rows = []
for ev in s['events']:
    k = ev['kind']
    if k not in ('gemCollection', 'gemLoss', 'gemSubmission', 'death', 'campClear'): continue
    team = ev['owner']['team'][-1]; who = ev['owner']['name'].split()[0][:6]
    for en in ev['entries']:
        r = en['timestamp']['round']
        if r > until: continue
        v = en.get('value'); t = en.get('target')
        if k == 'campClear' and (not isinstance(t, dict) or t.get('id') not in (0, 12, 13)): continue
        tag = {'gemCollection': 'coll', 'gemLoss': 'LOSS', 'gemSubmission': 'BANK', 'death': 'dead', 'campClear': 'clear'}[k]
        rows.append((r, f"r{r} {team}:{who} {tag} {v if k != 'death' else ''}{'' if k != 'campClear' else 'g' + str(t.get('id'))} @{en.get('x')},{en.get('y')}"))
rows.sort()
print(os.path.basename(d.rstrip('/')), len(rows), 'events')
for i in range(0, len(rows), 5): print('   ' + ' | '.join(x[1] for x in rows[i:i+5]))
