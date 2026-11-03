import sys
sys.path.insert(0, '.')
from qparse import load
# usage: near.py rounds.txt X Y R FROM TO [EVERY] -> champions within R of (X,Y) per round (pos hp gems act)
path, X, Y, R, a, b = sys.argv[1:7]; every = int(sys.argv[7]) if len(sys.argv) > 7 else 1
X, Y, R, a, b = int(X), int(Y), int(R), int(a), int(b)
for rnd, sub, st in load(path):
    if rnd < a or rnd > b or (rnd - a) % every: continue
    near = [(k, v) for k, v in st.items() if v and v['hp'] > 0 and max(abs(v['x'] - X), abs(v['y'] - Y)) <= R]
    if near: print(f"r{rnd} " + " | ".join(f"{k}@{v['x']},{v['y']} hp{v['hp']} g{v['g']} {v['act'][:30]}" for k, v in near))
