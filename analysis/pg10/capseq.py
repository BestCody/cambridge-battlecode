import sys, os, json, collections
import sys as _s; _s.stdout.reconfigure(encoding="utf-8", errors="replace")
# usage: capseq.py DIR SIDE UNTIL -> every camp clear by SIDE until round UNTIL: round, group (gems/gold), champion; then per-champion totals
d = sys.argv[1]; side = 'team' + sys.argv[2]; until = int(sys.argv[3])
s = json.load(open(os.path.join(d, 'summary.json'), encoding='utf-8'))
lay = {}
for l in open(os.path.join(d, 'layout.txt')):
    if l.startswith('group'):
        t = l.split(); lay[int(t[1])] = (int(t[2].split('=')[1]), int(t[3].split('=')[1]))
rows = []
for ev in s['events']:
    if ev['kind'] != 'campClear' or ev['owner']['team'] != side: continue
    for en in ev['entries']:
        r = en['timestamp']['round']
        if r <= until: rows.append((r, en['target']['id'], ev['owner']['name'].split()[0][:7]))
rows.sort()
per = collections.defaultdict(lambda: [0, 0, 0])
line = []
for r, g, who in rows:
    gems, gold = lay.get(g, (0, 0))
    line.append(f"r{r} g{g}({gems}/{gold}) {who}")
    per[who][0] += 1; per[who][1] += gems; per[who][2] += gold
print(os.path.basename(d.rstrip('/')), side, f"{len(rows)} clears to r{until}")
for i in range(0, len(line), 6): print('   ' + ' | '.join(line[i:i+6]))
print('   per champion (clears, gems, gold):', dict(per))
