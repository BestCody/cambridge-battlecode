import re,glob,os,json,collections,sys
sys.stdout.reconfigure(encoding='utf-8')
feat={}
for f in glob.glob('analysis/mapfeat/*.txt'):
    m=os.path.basename(f)[:-4]; groups=[]; spawns={}
    for line in open(f,encoding='utf-8'):
        g=re.match(r'group (\d+) gems=(\d+) gold=(\d+) respawn=(\d+)\s+(.*)',line)
        if g:
            camps=re.findall(r'(\w+)@(\d+),(\d+)',g.group(5))
            xs=[int(c[1]) for c in camps]; ys=[int(c[2]) for c in camps]
            hard=any(c[0] in ('elk','golem','monolith') for c in camps)
            groups.append(dict(gems=int(g.group(2)),gold=int(g.group(3)),x=sum(xs)/len(xs),y=sum(ys)/len(ys),hard=hard,kinds=set(c[0] for c in camps)))
        s=re.match(r'spawnA (\{.*?\}) spawnB (\{.*?\})',line)
        if s:
            a=json.loads(s.group(1)); b=json.loads(s.group(2))
            spawns={'A':((a['x0']+a['x1'])/2,(a['y0']+a['y1'])/2),'B':((b['x0']+b['x1'])/2,(b['y0']+b['y1'])/2)}
    if not spawns: continue
    ax,ay=spawns['A']; bx,by=spawns['B']
    sep=abs(ax-bx)+abs(ay-by)
    tg=sum(g['gems'] for g in groups); tgold=sum(g['gold'] for g in groups)
    own_gold=own_gems=mid_gems=mid_gold=hard_gems=0; near=999
    for g in groups:
        da=abs(g['x']-ax)+abs(g['y']-ay); db=abs(g['x']-bx)+abs(g['y']-by)
        near=min(near,da)
        if da<db*0.8: own_gold+=g['gold']; own_gems+=g['gems']
        if abs(da-db)<=0.2*sep: mid_gems+=g['gems']; mid_gold+=g['gold']
        if g['hard']: hard_gems+=g['gems']
    feat[m]=dict(groups=len(groups),gems=tg,gold=tgold,own_gold=own_gold,own_gems=own_gems,mid_gems=mid_gems,mid_gold=mid_gold,hard_gems=hard_gems,sep=int(sep),near=int(near))
res=collections.defaultdict(lambda:collections.Counter()); picks={}
for g in json.load(open('analysis/v106_140/games.json')):
    res[g['map']][g['opponent']]+=g['won']
for f in glob.glob('analysis/vesper_140/current-test-bot-100000-A-*.json'):
    m=os.path.basename(f)[len('current-test-bot-100000-A-'):-5]
    d=json.load(open(f)); picks[m]=len([c for c in d['champions'] if c['team']=='teamA'])
print('%-22s pk grp gems gold ownG ownGm midGm midG hardGm sep near | tb st'%'map')
rows=[]
for m in sorted(feat):
    x=feat[m]; rows.append((m,x))
    print('%-22s %2s %3d %4d %4d %4d %4d %4d %4d %4d %3d %3d | %d %d'%(m,picks.get(m,'?'),x['groups'],x['gems'],x['gold'],x['own_gold'],x['own_gems'],x['mid_gems'],x['mid_gold'],x['hard_gems'],x['sep'],x['near'],res[m]['test-bot'],res[m]['stacker']))
json.dump({m:dict(x,picks=picks.get(m),tb=res[m]['test-bot'],st=res[m]['stacker']) for m,x in feat.items()},open('analysis/mapfeat/features.json','w'),indent=0)
