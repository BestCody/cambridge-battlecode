import json,collections,sys
rows=[]
for d in sys.argv[1:]:
    rows+=json.load(open(f'analysis/{d}/games.json'))
w=collections.Counter(); n=collections.Counter(); k=collections.Counter(); dd=collections.Counter(); col=collections.Counter()
bym=collections.defaultdict(lambda: collections.Counter())
for x in rows:
    c=x['config']; n[c]+=1; w[c]+=x['won']; k[c]+=x['kills']; dd[c]+=x['deaths']; col[c]+=x['collected']; bym[x['map']][c]+=x['won']
for c in n: print('%-8s %3d/%3d kills %4d deaths %4d collected %6d'%(c,w[c],n[c],k[c],dd[c],col[c]))
weak=['00anvil','00collision','00galaxy','00interlock','00oasis','00quadrants','00reverse-spiral','00the-line','00vault','adgato-gemcombat4']
print('weak class:', {c: sum(bym[m][c] for m in weak) for c in n})
print('rest     :', {c: w[c]-sum(bym[m][c] for m in weak) for c in n})
cfgs=list(n)
print('%-22s'%'map',' '.join('%8s'%c for c in cfgs))
for m in sorted(bym):
    r=[bym[m][c] for c in cfgs]
    if len(set(r))>1: print('%-22s'%m,' '.join('%8d'%v for v in r))
