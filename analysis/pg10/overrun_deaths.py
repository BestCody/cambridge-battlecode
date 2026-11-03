import sys, os, json, re, glob, collections
# usage: overrun_deaths.py DIR... -> per game: our overruns, our deaths, deaths preceded (within W rounds) by an overrun of that champion
W = int(os.environ.get('W', '3'))
NAMES = {'Ryth':0,'Vesper Vale':1,'Vendetta':2,'Cassi':3,'Issac':4,'Stellar':5,'Prism':6,'Marcella':7,'Willow':8,'Varg':9,'Yuki':10,'Jester':11,'Heart of Ruin':12}
tot = collections.Counter()
for d in sys.argv[1:]:
    outs = glob.glob(os.path.join(d, 'team-*.stdout'))
    if not outs or not os.path.exists(os.path.join(d, 'summary.json')): continue
    seat = os.path.basename(outs[0])[5].upper()
    txt = open(outs[0], encoding='utf-8', errors='replace').read()
    over = collections.defaultdict(list)
    for s, e, t in re.findall(r'FUEL_OVERRUN started=(\d+) ended=(\d+) type=(\d+)', txt):
        over[int(t)].append((int(s), int(e)))
    summ = json.load(open(os.path.join(d, 'summary.json'), encoding='utf-8'))
    deaths = 0; hit = 0; unknown = 0
    for ev in summ['events']:
        if ev['kind'] != 'death' or ev['owner']['team'] != 'team' + seat: continue
        t = NAMES.get(ev['owner']['name'])
        if t is None: unknown += 1; continue
        for en in ev['entries']:
            r = en['timestamp']['round']; deaths += 1
            if any(r - W <= e <= r + 0 or s <= r <= e for s, e in over[t]): hit += 1
    n_over = sum(len(v) for v in over.values())
    tot['games'] += 1; tot['deaths'] += deaths; tot['hit'] += hit; tot['over'] += n_over
    print(f"{os.path.basename(d):24} seat {seat} overruns {n_over:3} deaths {deaths:3} preceded-by-overrun {hit:3}  unknown-names {unknown}")
g = max(1, tot['games'])
exp = tot['over'] / g * (W + 1) / 1200 / 5  # chance a given champion-round sits in an overrun window
print(f"TOTAL games {tot['games']} overruns {tot['over']} ({tot['over']/g:.1f}/game) deaths {tot['deaths']} preceded {tot['hit']} = {tot['hit']/max(1,tot['deaths']):.1%}; chance level about {exp:.1%}")
