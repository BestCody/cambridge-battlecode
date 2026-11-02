# usage: tree.py REF_DIR REF_CFG NEW_DIR NEW_CFG -> greedy depth-2 tree on map features for "new beats ref"
import json,collections,sys,itertools
def tab(d,cfg):
    g=json.load(open(d+'/games.json')); w=collections.Counter()
    for x in g:
        if x['config']==cfg: w[x['map']]+=x['won']
    return w
a=tab(sys.argv[1],sys.argv[2]); b=tab(sys.argv[3],sys.argv[4])
feat=json.load(open('analysis/mapfeat/features.json'))
maps=sorted(feat)
def fx(m):
    f=feat[m]; g=max(1,f['gems'])
    return {'picks':f['picks'] or 0,'mid_share':f['mid_gems']/g,'own_gold':f['own_gold'],'gold':f['gold'],'gems':f['gems'],
            'hard_share':f['hard_gems']/g,'sep':f['sep'],'near':f['near'],'groups':f['groups'],'own_gems':f['own_gems'],'gold_per_gem':f['gold']/g,'mid_gold':f['mid_gold']}
X={m:fx(m) for m in maps}; delta={m:b[m]-a[m] for m in maps}
def best_split(ms):
    best=None
    for k in X[ms[0]]:
        vals=sorted(set(X[m][k] for m in ms))
        for lo,hi in zip(vals,vals[1:]):
            t=(lo+hi)/2
            for side in ('<=','>'):
                cls=[m for m in ms if (X[m][k]<=t)==(side=='<=')]
                if len(cls)<4: continue
                tot=sum(delta[m] for m in cls); up=sum(delta[m]>0 for m in cls); down=sum(delta[m]<0 for m in cls)
                score=tot-1.0*down  # total gain, penalised by members that lose
                if best is None or score>best[0]: best=(score,k,side,t,cls,tot,up,down)
    return best
def show(b,ind=''):
    score,k,side,t,cls,tot,up,down=b
    print('%s%s %s %.3g: %d maps, total %+d, better %d, worse %d'%(ind,k,side,t,len(cls),tot,up,down))
    print('%s   %s'%(ind,' '.join('%s(%+d)'%(m,delta[m]) for m in cls)))
    return cls
root=best_split(maps); cls=show(root)
rest=[m for m in maps if m not in cls]
print('-- inside the class, a second split:'); sub=best_split(cls); show(sub,'  ')
print('-- outside the class:'); other=best_split(rest); show(other,'  ')
print('hand-routed set total %+d'%sum(delta[m] for m in ['00anvil','00dunes','00gemfight','00oasis','00the-line','01boss-battle','01broken-curve','01islands','01yellow-brick-road','02geodes','02tangle','adgato-gemcombat1']))
