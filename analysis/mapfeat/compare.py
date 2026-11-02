# usage: compare.py REF_DIR REF_CFG NEW_DIR NEW_CFG -> per-map wins (test-bot, stacker) and deaths, maps that differ
import json,collections,sys
def tab(d,cfg):
    g=json.load(open(d+'/games.json')); w=collections.Counter(); dth=collections.Counter()
    for x in g:
        if x['config']!=cfg: continue
        key=(x['map'],x['opponent']); w[key]+=x['won']; dth[key]+=x['deaths']
    return w,dth
a=tab(sys.argv[1],sys.argv[2]); b=tab(sys.argv[3],sys.argv[4])
feat=json.load(open('analysis/mapfeat/features.json'))
rows=[]
for m in sorted(feat):
    tb=(m,'test-bot'); st=(m,'stacker')
    rows.append(((b[0][tb]+b[0][st])-(a[0][tb]+a[0][st]),m,a[0][tb],a[0][st],b[0][tb],b[0][st],a[1][tb]+a[1][st],b[1][tb]+b[1][st],feat[m]))
rows.sort(reverse=True)
print('%-22s pk mid%% ownG | ref tb st | new tb st | deaths ref new'%'map')
for d,m,a1,a2,b1,b2,d1,d2,f in rows:
    if d!=0: print('%-22s %s %3.0f%% %5d | %d %d | %d %d | %3d %3d  %s'%(m,f['picks'],100*f['mid_gems']/max(1,f['gems']),f['own_gold'],a1,a2,b1,b2,d1,d2,'+%d'%d if d>0 else d))
print('better',sum(1 for r in rows if r[0]>0),'worse',sum(1 for r in rows if r[0]<0),'same',sum(1 for r in rows if r[0]==0))
print('totals ref %d new %d of %d'%(sum(r[2]+r[3] for r in rows),sum(r[4]+r[5] for r in rows),4*len(rows)))
