"""English play-by-play of one team from a rounds.mjs dump and a layout.mjs dump.

usage: narrate.py rounds.txt layout.txt TEAM(A|B) FROM TO [summary.json]
Each champion's turns become segments of the same activity ("farming gold cluster G3 (31,19)",
"walking toward G8", "standing idle beside G5", "fighting Vendetta"), merged while unchanged.
Idle = no move, no attack and no ability for the round, while alive and not channelling."""
import json, re, sys
sys.path.insert(0, __import__("os").path.dirname(__file__))
from qparse import load, d

rounds_path, layout_path, team, lo, hi = sys.argv[1:6]
lo, hi = int(lo), int(hi)
summary = json.load(open(sys.argv[6])) if len(sys.argv) > 6 else None
enemy = "B" if team == "A" else "A"

groups = []
for line in open(layout_path):
    m = re.match(r"group (\d+) gems=(\d+) gold=(\d+) respawn=(\d+)\s+(.*)", line)
    if not m:
        continue
    camps = [(k, int(x), int(y)) for k, x, y in re.findall(r"(\w+)@(\d+),(\d+)", m[5])]
    kinds = sorted({k for k, _, _ in camps})
    label = f"G{m[1]}[{'+'.join(kinds)} {m[2]}gem/{m[3]}g]"
    groups.append((label, camps))
spawn = re.search(r'spawn' + team + r' \{"x0":(\d+),"y0":(\d+),"x1":(\d+),"y1":(\d+)\}', open(layout_path).read())
sx0, sy0, sx1, sy1 = map(int, spawn.groups()) if spawn else (0, 0, -1, -1)


def near_group(x, y, r=3):
    best = None
    for label, camps in groups:
        dist = min(max(abs(x - cx), abs(y - cy)) for _, cx, cy in camps)
        if dist <= r and (best is None or dist < best[1]):
            best = (label, dist)
    return best


def camp_at(x, y):
    for label, camps in groups:
        if any(cx == x and cy == y for _, cx, cy in camps):
            return label
    return None


rows = load(rounds_path)
events = {}
if summary:
    for e in summary["events"]:
        if e["kind"] not in ("death", "gemSubmission", "gemCollection", "buyItem", "itemCombination", "kill"):
            continue
        o = e["owner"]
        if not isinstance(o, dict) or not o.get("name"):
            continue
        for x in e["entries"]:
            r = x["timestamp"]["round"]
            tgt = x.get("target") or {}
            name = tgt.get("name", "") if isinstance(tgt, dict) else ""
            what = {"death": f"DIES (killer {name})", "kill": f"kills {name}", "gemSubmission": f"tributes {x['value']} gems",
                    "gemCollection": f"captures {x['value']} gems", "buyItem": f"buys {name}",
                    "itemCombination": f"completes {name}"}[e["kind"]]
            side = "A" if o.get("team") == "teamA" else "B"
            events.setdefault((o["name"], side), []).append((r, what))

names = sorted({k[:-1] for _, _, st in rows for k in st if k.endswith(team)})
TYPEN = {"HoR": "Heart of Ruin", "Vesper": "Vesper Vale"}
prev_pos = {}
for champ in names:
    key = champ + team
    segs = []
    for rnd, sub, st in rows:
        if rnd < lo or rnd > hi:
            continue
        v = st.get(key)
        if v is None or v["hp"] <= 0:
            label = "dead"
        else:
            enemies = [(k[:-1], e) for k, e in st.items() if k.endswith(enemy) and e and e["hp"] > 0]
            close = [n for n, e in enemies if d(v, e) <= 4]
            acts = v["act"]
            hits = re.findall(r"(atk|abl\d)>(\d+),(\d+)", acts)
            target = None
            for kind, x, y in hits:
                x, y = int(x), int(y)
                for n, e in enemies:
                    if e["x"] == x and e["y"] == y:
                        target = f"hits {n}"
                c = camp_at(x, y)
                if not target and c:
                    target = f"farms {c}"
            moved = "mv>" in acts
            g = near_group(v["x"], v["y"])
            at_spawn = sx0 <= v["x"] <= sx1 and sy0 <= v["y"] <= sy1
            if target:
                label = target + (f" [enemies near: {','.join(close)}]" if close and not target.startswith("hits") else "")
            elif at_spawn and not moved:
                label = "waiting at spawn"
            elif not moved and not hits:
                label = f"IDLE beside {g[0]}" if g else f"IDLE at {v['x']},{v['y']}"
                if close:
                    label += f" [enemies near: {','.join(close)}]"
            else:
                label = f"walking near {g[0]}" if g else "walking"
                if close:
                    label += f" [enemies near: {','.join(close)}]"
        if segs and segs[-1][2] == label:
            segs[-1][1] = rnd
        else:
            segs.append([rnd, rnd, label, v])
    print(f"\n### {champ} ({'us' if team else ''} team {team})")
    evs = sorted(events.get((TYPEN.get(champ, champ), team), []))
    for a, b, label, v in segs:
        span = f"r{a}" if a == b else f"r{a}-{b}"
        hp = f" hp{v['hp']} g{v['g']}" if v and v["hp"] > 0 else ""
        ev = [w for r, w in evs if a <= r <= b]
        print(f"  {span:10} {label}{hp}{('  << ' + '; '.join(ev)) if ev else ''}")
