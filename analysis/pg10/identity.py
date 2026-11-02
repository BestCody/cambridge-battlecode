import sys, json, subprocess, re
# usage: identity.py GROUP_ID RESULTS_FILE  -> per map: gems of the group game vs the reference game (same map in the results file)
g, ref = sys.argv[1], sys.argv[2]
d = json.loads(subprocess.run(['cambc','match-groups','show',g,'--json'],capture_output=True,text=True).stdout)
refs = {}
for l in open(ref, encoding='utf-8'):
    m = re.match(r'^\s+(\S+)\s+finished\s+(\w+)\s+\S*\s+(\S+)', l)
    if m: refs[m[1]] = (m[2], m[3])
def gems(mid):
    s = json.loads(subprocess.run(['cambc','matches','summary',mid,'--json','-o','-'],capture_output=True,text=True).stdout or '{}')
    try:
        tl = s['teamTimelines']; return tuple(sorted((k, v[-1] if isinstance(v, list) else v) for k, v in s.get('match', {}).items() if 'ems' in k)) or None
    except Exception: return None
for m in d['matches']:
    us = [t for t in m['teams'] if t['name']=='The Party of Heroes'][0]
    r = refs.get(m['map'])
    print(m['map'].ljust(22), 'now', us['result'], '| v79', r[0] if r else '?', '| ids', m['matchId'][:8], (r[1][:8] if r else ''))
