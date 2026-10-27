"""Pick fights to watch from replay dumps (rounds 0-maxround), by kind:
  A  our chase where one of our chasers died;   A+ their chase that killed ours without losing a chaser
  B  our death in a fight we outnumbered at contact (within 6);   B+ their kill when they outnumbered at contact, no loss
  C  our retreat that ended in our death;   C+ their retreat that escaped while we chased
A retreat: 3+ rounds in a row stepping towards its own spawn with an enemy
within 5; chasers: enemies within 5 of it for 3+ of the following rounds.
Contact: the run of rounds with an enemy within 6 before a death. Spawn tiles
are where each team's champions stand when first placed.
Input: a jobs file, one game per line: <replay_dump.mjs output .jsonl> <our
team 0/1> <map> <summary.json> <opponent>.
Prints one line per fight: kind, match, our team, map, opponent, focus champion, rounds to watch.
usage: python -I -X utf8 select_fights.py <jobs.txt> [maxround]"""
import json, os, sys
from collections import defaultdict

cheb = lambda a, b: max(abs(a[0] - b[0]), abs(a[1] - b[1]))
maxround = int(sys.argv[2]) if len(sys.argv) > 2 else 300
found = defaultdict(list)
for line in open(sys.argv[1], encoding="utf-8"):
    p = line.split()
    if len(p) < 5:
        continue
    dump, team, mp, opp = p[0], int(p[1]), p[2], p[4]
    if not os.path.exists(dump):
        continue
    rows = [json.loads(r) for r in open(dump, encoding="utf-8") if r.startswith("{")]
    n = len(rows)
    spawn = {0: [], 1: []}
    for r in rows:
        for c in r["c"]:
            if c["x"] >= 0 and not spawn[c["team"]]:
                spawn[c["team"]] = [(o["x"], o["y"]) for o in r["c"] if o["team"] == c["team"] and o["x"] >= 0]
        if spawn[0] and spawn[1]:
            break
    missing = [t for t in (0, 1) if not spawn[t]]
    if missing:
        sys.exit(f"{dump}: team {missing[0]} never stands on the map; the dump is incomplete")
    pos = [{c["id"]: c for c in r["c"]} for r in rows]
    ids = sorted({cid for p_ in pos for cid in p_})
    home = lambda c: min(cheb((c["x"], c["y"]), s) for s in spawn[c["team"]])
    foes = lambda i, c, d: [o for o in rows[i]["c"] if o["alive"] and o["team"] != c["team"] and cheb((c["x"], c["y"]), (o["x"], o["y"])) <= d]
    mates = lambda i, c, d: [o for o in rows[i]["c"] if o["alive"] and o["team"] == c["team"] and o["id"] != c["id"] and cheb((c["x"], c["y"]), (o["x"], o["y"])) <= d]
    tag = (dump, team, mp, opp)
    # Retreats and chases.
    for cid in ids:
        run = 0
        for i in range(1, n):
            if rows[i]["r"] > maxround:
                break
            c, b = pos[i].get(cid), pos[i - 1].get(cid)
            if not c or not c["alive"] or not b or not b["alive"]:
                run = 0
                continue
            run = run + 1 if home(c) < home(b) and (foes(i, c, 5) or run) else 0
            if run != 3:
                continue
            s0 = i - 2
            inreach, died, k = defaultdict(int), False, s0 + 1
            while k < min(n, s0 + 26):
                ck = pos[k].get(cid)
                if not ck or not ck["alive"]:
                    died = True
                    break
                for o in foes(k, ck, 5):
                    inreach[o["id"]] += 1
                if not foes(k, ck, 8):
                    break
                k += 1
            chasers = [o for o, t in inreach.items() if t >= 3]
            if not chasers:
                continue
            kk = min(k, n - 1)
            lost = [o for o in chasers if not (pos[kk].get(o) or {}).get("alive", False)]
            ours = c["team"] == team
            span = (rows[max(0, s0 - 6)]["r"], rows[kk]["r"] + 1)
            if not ours and lost:
                found["A"].append((*tag, lost[0], *span, f"we chased their {cid}; lost {len(lost)} of {len(chasers)} chasers; retreater {'died' if died else 'lived'}"))
            if ours and died and not lost:
                found["A+"].append((*tag, cid, *span, f"they chased our {cid} with {len(chasers)} and killed it, no loss"))
            if ours and died:
                found["C"].append((*tag, cid, *span, f"our {cid} retreated at {pos[s0][cid]['hp']}/{pos[s0][cid]['maxHp']} and died; {len(chasers)} chasers, {len(lost)} lost"))
            if not ours and not died:
                found["C+"].append((*tag, cid, *span, f"their {cid} retreated at {pos[s0][cid]['hp']}/{pos[s0][cid]['maxHp']} from {len(chasers)} of ours and lived"))
    # Deaths after contact.
    for i in range(1, n):
        if rows[i]["r"] > maxround:
            break
        for c in rows[i]["c"]:
            was = pos[i - 1].get(c["id"])
            if not (was and was["alive"] and not c["alive"]):
                continue
            if not foes(i - 1, was, 6):
                continue
            start = i - 1
            while start - 1 >= 0 and i - start < 25:
                me = pos[start - 1].get(c["id"])
                if not me or not me["alive"] or not foes(start - 1, me, 6):
                    break
                start -= 1
            me0 = pos[start][c["id"]]
            n_side, n_foe = len(mates(start, me0, 6)) + 1, len(foes(start, me0, 6))
            # Losses of the killing side during the fight.
            killer_lost = sum(1 for o in rows[i]["c"] if o["team"] != c["team"] and not o["alive"] and (pos[start].get(o["id"]) or {}).get("alive"))
            span = (rows[max(0, start - 6)]["r"], rows[i]["r"] + 2)
            if c["team"] == team and n_side > n_foe:
                found["B"].append((*tag, c["id"], *span, f"our {c['id']} died: {n_side} of ours vs {n_foe} at contact, {i - start} rounds; they lost {killer_lost}"))
            if c["team"] == team and n_foe > n_side and not killer_lost:
                found["B+"].append((*tag, c["id"], *span, f"they killed our {c['id']}: {n_foe} vs {n_side} at contact, {i - start} rounds, no loss"))
for kind in ("A", "A+", "B", "B+", "C", "C+"):
    fs = found[kind]
    print(f"== {kind}: {len(fs)}")
    for f in sorted(fs, key=lambda f: f[5])[:12]:
        d, t, mp, opp, cid, r0, r1, note = f
        print(f"   {kind:2} {os.path.basename(d)[:8]} {t} {mp:22} {opp[:12]:12} focus {cid:2} rounds {r0:4}-{r1:4}  {note}")
