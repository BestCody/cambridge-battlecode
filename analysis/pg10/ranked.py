import json,subprocess,sys,collections
import sys; sys.stdout.reconfigure(encoding="utf-8",errors="replace")
n=int(sys.argv[1]) if len(sys.argv)>1 else 60
out=subprocess.run(['cambc','matches','list','--team','me','--kind','ranked','--limit',str(n),'--json'],capture_output=True,stdin=subprocess.DEVNULL).stdout.decode("utf-8","replace")
ms=json.loads(out)['matches']
groups=collections.OrderedDict()
for m in ms:
    us=[t for t in m['teams'] if t['name']=='The Party of Heroes'][0]
    them=[t for t in m['teams'] if t['name']!='The Party of Heroes'][0]
    g=groups.setdefault(m['groupId'],{'t':m['createdAt'][11:16],'opp':them['name'],'w':0,'n':0,'maps':[]})
    r=us.get('result') or 'pending'; g['n']+=1; g['w']+=r=='win'; g['maps'].append((m['map'],r[0],us['side'],m['matchId']))
for gid,g in groups.items():
    print('%s %-22s %d/%d  %s'%(g['t'],g['opp'][:22],g['w'],g['n'],' '.join('%s:%s'%(a,b) for a,b,c,d in g['maps'])))
print('total %d/%d'%(sum(g['w'] for g in groups.values()),sum(g['n'] for g in groups.values())))
