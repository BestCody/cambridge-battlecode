import re
def load(path):
    rows=[]
    for l in open(path):
        if not l.startswith("r") or "|" not in l: continue
        head,*parts=l.strip().split(" | ")
        rnd=int(head.split()[0][1:]); sub=head.split()[1:]
        st={}
        for p in parts:
            if "=" not in p: continue
            name,rest=p.split("=",1)
            m=re.match(r"(-?\d+),(-?\d+) hp(-?\d+) g(-?\d+) s(\w+)(.*)",rest)
            st[name]=None if not m else dict(x=int(m[1]),y=int(m[2]),hp=int(m[3]),g=int(m[4]),s=int(m[5],16),act=m[6].strip())
        rows.append((rnd,sub,st))
    rows.sort(key=lambda r:r[0])
    return rows
def d(a,b): return max(abs(a["x"]-b["x"]),abs(a["y"]-b["y"]))
