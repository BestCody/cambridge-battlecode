"""Macro-level summary of how print dont int plays, from the extracted turn records.

    python training/macro_report.py            # -> training/reports/macro/report.md + *.png

Covers results and drafts, how each hero spends its turns over a game, recall and tribute habits,
item build orders, target choice, teleports, wards and how tightly the team groups up.
"""
import csv
import gzip
import json
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from tqdm import tqdm

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402

TRAINING = Path(__file__).resolve().parent
OUT = TRAINING / "reports" / "macro"
NAMES = {0: "Ryth", 1: "Vesper", 2: "Vendetta", 7: "Marcella", 12: "HoR"}
DRAFT_NAMES = ["Ryth", "Vesper", "Vendetta", "Cassi&Issac", "Stellar", "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi", "HoR"]
CAMP_NAMES = ["wolf", "elk", "gem", "gold", "monolith", "golem"]
FAMILIES = ["monster", "power", "durability", "treasure", "jungle", "home", "bruiser", "tribute", "capture", "prospector",
            "hunter", "skirmish", "greed", "collectors"]
ACTIVITIES = ["dead", "recall channel", "tribute channel", "fight champion", "farm camp", "move", "idle"]
BUCKET = 100   # rounds per time bucket


def item_name(i):
    if i >= 84:
        return ["tp_crystal", "control_ward", "altar_lock", "tp_scroll"][i - 84]
    fam, rest = divmod(i, 6)
    return f"{FAMILIES[fam]}_{['str', 'foc', 'hp'][rest // 2]}"


def analyse(path):
    lines = gzip.open(path, "rt", encoding="utf-8").read().splitlines()
    head = json.loads(lines[0])
    h = head["header"]
    recs = [json.loads(l) for l in lines[1:]]
    if len(recs) < 200:
        return None
    team = h["team"]
    types = {c["id"]: c["type"] for c in h["champions"]}
    camp_type = {c["handle"]: c["type"] for g in h["groups"] for c in g["camps"]}
    altars = [(x, y) for y, r in enumerate(h["altars"]) for x, ch in enumerate(r) if ch == "1"]
    out = {"activity": defaultdict(Counter), "recall_commit": defaultdict(list), "tributes": defaultdict(list),
           "buys": defaultdict(list), "camp_attacks": defaultdict(Counter), "attack_kind": defaultdict(Counter),
           "target_rank": defaultdict(Counter), "teleports": defaultdict(list), "wards": defaultdict(Counter),
           "abilities": defaultdict(Counter), "deaths": Counter(), "spread": defaultdict(list),
           "first_tribute": {}, "rounds": head["final"]["round"], "submitted": head["final"]["submitted"],
           "team": team, "dash_toward": defaultdict(Counter)}
    by_round = defaultdict(dict)
    prev_alive = {}
    ms = F.MapStatic(h)
    future = {}
    by_champ = defaultdict(list)
    for i, r in enumerate(recs):
        by_champ[r["c"]].append(i)
    for idx in by_champ.values():
        for k, a in enumerate(idx):
            future[a] = [recs[b] for b in idx[k + 1:k + 8]]
    for i, r in enumerate(recs):
        hero = NAMES.get(types[r["c"]], str(types[r["c"]]))
        o, s = r["obs"], r["obs"]["self"]
        bucket = r["r"] // BUCKET
        kinds = [st["act"]["k"] for st in r["steps"]]
        alive = bool(s.get("alive", 1)) and s.get("vis", 0)
        if prev_alive.get(r["c"], True) and not alive:
            out["deaths"][hero] += 1
        prev_alive[r["c"]] = alive
        st = s.get("st", 0) or 0
        if alive:
            by_round[r["r"]][r["c"]] = (s["x"], s["y"])
        # Activity of the turn (first match wins).
        if not alive:
            act = "dead"
        elif st & F.ST_RECALLING and F.K_CANCEL not in kinds:
            act = "recall channel"
        elif st & F.ST_SUBMITTING and F.K_CANCEL not in kinds or F.K_SUBMIT in kinds:
            act = "tribute channel"
        else:
            attacks = [st_["act"] for st_ in r["steps"] if st_["act"]["k"] in (F.K_ATTACK, F.K_ABILITY)]
            ecoords = {(e["x"], e["y"]) for e in o["enemies"] if e.get("vis")}
            if any((a["tx"], a["ty"]) in ecoords for a in attacks):
                act = "fight champion"
            elif attacks:
                act = "farm camp"
            elif F.K_MOVE in kinds:
                act = "move"
            else:
                act = "idle"
        out["activity"][hero][(bucket, act)] += 1
        if not alive:
            continue
        x, y = s["x"], s["y"]
        # Recalls that actually brought the champion home (most are speculative and cancelled).
        if F.K_RECALL in kinds:
            if F.recall_arrives(ms, r, future[i]):
                out["recall_commit"][hero].append((s["hp"] / max(s["mhp"], 1), o.get("gems") or 0, o.get("gold") or 0, r["r"]))
        if F.K_SUBMIT in kinds:
            out["tributes"][hero].append((o.get("gems") or 0, r["r"]))
            out["first_tribute"].setdefault(hero, r["r"])
        for st_ in r["steps"]:
            a = st_["act"]
            if a["k"] == F.K_BUY and "after" in a:
                before = st_.get("econ") or o
                lab = F.shop_item_label(before, a["after"])
                if lab is not None:
                    out["buys"][hero].append(lab)
            if a["k"] == F.K_ATTACK:
                cls = F._target_class(r, x, y, a["tx"], a["ty"])
                out["attack_kind"][hero][F.TARGET_CLASSES[cls]] += 1
                for c in o["camps"]:
                    if (c[1], c[2]) == (a["tx"], a["ty"]):
                        out["camp_attacks"][hero][CAMP_NAMES[camp_type.get(c[0], 0)]] += 1
                # Was it the lowest-health camp in range, or simply the nearest?
                in_range = [c for c in o["camps"] if c[3] > 0 and F.cheb(x, y, c[1], c[2]) <= 3]
                if len(in_range) > 1:
                    low = min(in_range, key=lambda c: c[3])
                    near = min(in_range, key=lambda c: F.cheb(x, y, c[1], c[2]))
                    tgt = (a["tx"], a["ty"])
                    out["target_rank"][hero]["lowest hp" if tgt == (low[1], low[2]) else
                                             "nearest" if tgt == (near[1], near[2]) else "other"] += 1
            if a["k"] == F.K_ABILITY:
                out["abilities"][hero][f"slot{a.get('slot')}_type{a.get('at')}"] += 1
                if a.get("at") == 6:
                    # Dash relative to home: towards spawn, towards altar or neither?
                    home = h["spawns"][team]
                    hx, hy = (home["x0"] + home["x1"]) / 2, (home["y0"] + home["y1"]) / 2
                    d0, d1 = abs(a["fx"] - hx) + abs(a["fy"] - hy), abs(a["tx"] - hx) + abs(a["ty"] - hy)
                    hp = s["hp"] / max(s["mhp"], 1)
                    near_enemy = any(e.get("vis") and F.cheb(x, y, e["x"], e["y"]) <= 5 for e in o["enemies"])
                    out["dash_toward"][hero][("home" if d1 < d0 - 1 else "away") + (" (enemy near)" if near_enemy else "")
                                             + (" low hp" if hp < .35 else "")] += 1
            if a["k"] in (F.K_CONSUMABLE, F.K_TP_CHANNEL):
                e = st_.get("econ") or {"x": x, "y": y}
                ex, ey = e.get("x", x), e.get("y", y)
                out["teleports"][hero].append({"round": r["r"], "from": (ex, ey), "to": (a["tx"], a["ty"]),
                                               "gems": o.get("gems") or 0, "hp": s["hp"] / max(s["mhp"], 1)})
            if a["k"] == F.K_WARD:
                near = altars and min(F.cheb(a["tx"], a["ty"], ax, ay) for ax, ay in altars) <= 4
                out["wards"][hero]["near altar" if near else "elsewhere"] += 1
    # How tightly do they group? mean pairwise Chebyshev distance of alive champions per round bucket.
    for rnd, pos in by_round.items():
        p = list(pos.values())
        if len(p) >= 2:
            d = [F.cheb(*a, *b) for k, a in enumerate(p) for b in p[k + 1:]]
            out["spread"][rnd // BUCKET].append(float(np.mean(d)))
    return _plain(out)   # plain dicts travel back from worker processes


def _plain(o):
    if isinstance(o, (defaultdict, Counter, dict)):
        return {(k if isinstance(k, (str, int, tuple)) else str(k)): _plain(v) for k, v in o.items()}
    if isinstance(o, list):
        return [_plain(v) for v in o]
    return o


def main():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    OUT.mkdir(parents=True, exist_ok=True)
    games = list(csv.DictReader(open(TRAINING / "data" / "games.csv", encoding="utf-8")))
    raws = sorted((TRAINING / "data" / "raw").glob("*.jsonl.gz"))
    with ProcessPoolExecutor() as pool:
        results = [r for r in tqdm(pool.map(analyse, raws, chunksize=4), total=len(raws), desc="analysing", unit=" games") if r]

    md = ["# print dont int: how they play", "",
          f"Built from {len(results)} replays ({len(games)} downloaded, newest first) by `training/macro_report.py`.", ""]

    # ---- results ----
    wins = sum(g["result"] == "win" for g in games)
    md += ["## Results", "", f"- Record: **{wins} W / {len(games) - wins} L** ({wins / len(games):.0%})"]
    by_kind = defaultdict(Counter)
    for g in games:
        by_kind[g["kind"]][g["result"]] += 1
    for k, c in by_kind.items():
        md.append(f"- {k}: {c['win']} W / {c['loss']} L")
    reasons = Counter(g["endReason"] for g in games)
    md.append("- End reasons: " + ", ".join(f"{k} {v}" for k, v in reasons.most_common()))
    lengths = [r["rounds"] for r in results]
    md.append(f"- Game length: median {np.median(lengths):.0f} rounds (p10 {np.percentile(lengths, 10):.0f}, p90 {np.percentile(lengths, 90):.0f})")
    by_map = defaultdict(Counter)
    for g in games:
        by_map[g["map"]][g["result"]] += 1
    worst = sorted(by_map.items(), key=lambda kv: kv[1]["win"] / max(sum(kv[1].values()), 1))[:8]
    md += ["", "Weakest maps (W/L):", ""] + [f"- {m}: {c['win']}/{c['loss']}" for m, c in worst if sum(c.values()) >= 3]
    opp = defaultdict(Counter)
    for g in games:
        opp[g["opponent"]][g["result"]] += 1
    md += ["", "Most frequent opponents (W/L):", ""]
    md += [f"- {o}: {c['win']}/{c['loss']}" for o, c in sorted(opp.items(), key=lambda kv: -sum(kv[1].values()))[:10]]

    # ---- draft ----
    picks = Counter(g["picks"] for g in games)
    md += ["", "## Draft", "", "Pick order (draft ids) and how often:", ""]
    md += [f"- {' > '.join(DRAFT_NAMES[int(p)] for p in k.split('-'))}: {v}" for k, v in picks.most_common(6)]

    heroes = sorted({h for r in results for h in r["activity"]})

    # ---- activity over time ----
    fig, axes = plt.subplots(1, len(heroes), figsize=(4 * len(heroes), 3.6), sharey=True)
    colors = ["#9D9DA1", "#4C78A8", "#B279A2", "#E45756", "#54A24B", "#F2CF5B", "#D4D4D4"]
    md += ["", "## How each hero spends its turns", "", "![activity](activity.png)", "",
           "| hero | " + " | ".join(ACTIVITIES) + " |", "|---|" + "---|" * len(ACTIVITIES)]
    for ax, hero in zip(np.atleast_1d(axes), heroes):
        acc = Counter()
        for r in results:
            for k, v in r["activity"].get(hero, {}).items():
                acc[k] += v
        buckets = sorted({b for b, _ in acc})
        tot = {b: sum(acc[(b, a)] for a in ACTIVITIES) for b in buckets}
        bottom = np.zeros(len(buckets))
        for a, col in zip(ACTIVITIES, colors):
            vals = np.array([acc[(b, a)] / max(tot[b], 1) for b in buckets])
            ax.bar([b * BUCKET for b in buckets], vals, BUCKET * .9, bottom=bottom, color=col, label=a, align="edge")
            bottom += vals
        ax.set_title(hero); ax.set_xlabel("round")
        overall = sum(tot.values())
        md.append(f"| {hero} | " + " | ".join(f"{sum(acc[(b, a)] for b in buckets) / overall:.0%}" for a in ACTIVITIES) + " |")
    np.atleast_1d(axes)[0].set_ylabel("share of turns")
    np.atleast_1d(axes)[-1].legend(fontsize=7, loc="upper right")
    fig.tight_layout(); fig.savefig(OUT / "activity.png", dpi=110); plt.close(fig)

    # ---- recalls / tributes ----
    md += ["", "## Recalls and tributes", "",
           "They start a recall at the end of most turns and cancel it at the start of the next turn if they "
           "want to act (a *speculative* recall: the first channel tick is free if they decide to go home). "
           "Below, only recalls that really brought the champion home (it reappears on its spawn).", "",
           "| hero | trips home / game | median hp at trip | median gems carried | median gold | tributes / game | median gems per tribute | median first tribute round |",
           "|---|---|---|---|---|---|---|---|"]
    fig, axes = plt.subplots(1, 2, figsize=(11, 3.6))
    for hero in heroes:
        rc = [t for r in results for t in r["recall_commit"].get(hero, [])]
        tb = [t for r in results for t in r["tributes"].get(hero, [])]
        ft = [r["first_tribute"][hero] for r in results if hero in r["first_tribute"]]
        n = sum(1 for r in results if hero in r["activity"])
        md.append(f"| {hero} | {len(rc) / max(n, 1):.1f} | {np.median([t[0] for t in rc]) if rc else float('nan'):.2f} | "
                  f"{np.median([t[1] for t in rc]) if rc else float('nan'):.0f} | {np.median([t[2] for t in rc]) if rc else float('nan'):.0f} | "
                  f"{len(tb) / max(n, 1):.1f} | {np.median([t[0] for t in tb]) if tb else float('nan'):.0f} | {np.median(ft) if ft else float('nan'):.0f} |")
        if rc:
            axes[0].hist([t[0] for t in rc], bins=20, range=(0, 1), histtype="step", lw=1.5, label=hero)
        if tb:
            axes[1].hist([t[0] for t in tb], bins=30, range=(0, 300), histtype="step", lw=1.5, label=hero)
    axes[0].set_title("hp fraction when they really go home"); axes[0].legend(fontsize=8)
    axes[1].set_title("gems carried when starting a tribute"); axes[1].legend(fontsize=8)
    fig.tight_layout(); fig.savefig(OUT / "recall_tribute.png", dpi=110); plt.close(fig)
    md += ["", "![recalls and tributes](recall_tribute.png)"]

    # ---- builds ----
    md += ["", "## Item builds", "", "Most bought items per hero (shards; emblems complete automatically every 5), "
           "and the most common opening sequence of the first 8 purchases:", ""]
    for hero in heroes:
        allb = Counter(item_name(i) for r in results for i in r["buys"].get(hero, []))
        openings = Counter(" > ".join(item_name(i) for i in r["buys"].get(hero, [])[:8]) for r in results if r["buys"].get(hero))
        md.append(f"- **{hero}**: " + ", ".join(f"{k} {v}" for k, v in allb.most_common(8)))
        if openings:
            seq, cnt = openings.most_common(1)[0]
            md.append(f"  - opening ({cnt}/{sum(openings.values())} games): {seq}")

    # ---- targets ----
    md += ["", "## Targets", "", "| hero | attacks: camps vs champions | camp types hit | in-range pick: lowest hp / nearest / other |",
           "|---|---|---|---|"]
    for hero in heroes:
        ak = Counter(); ct = Counter(); tr = Counter()
        for r in results:
            ak.update(r["attack_kind"].get(hero, {})); ct.update(r["camp_attacks"].get(hero, {})); tr.update(r["target_rank"].get(hero, {}))
        champs = sum(v for k, v in ak.items() if k.startswith("enemy"))
        camps = sum(v for k, v in ak.items() if k.startswith("camp"))
        md.append(f"| {hero} | {camps} / {champs} (locks {ak.get('altar_lock', 0)}) | "
                  + ", ".join(f"{k} {v / max(sum(ct.values()), 1):.0%}" for k, v in ct.most_common()) + " | "
                  + " / ".join(f"{tr.get(k, 0) / max(sum(tr.values()), 1):.0%}" for k in ("lowest hp", "nearest", "other")) + " |")

    # ---- abilities, dashes ----
    md += ["", "## Abilities", ""]
    for hero in heroes:
        ab = Counter(); dt = Counter()
        for r in results:
            ab.update(r["abilities"].get(hero, {})); dt.update(r["dash_toward"].get(hero, {}))
        md.append(f"- **{hero}**: " + ", ".join(f"{k} {v}" for k, v in ab.most_common()))
        if dt:
            md.append("  - dashes: " + ", ".join(f"{k} {v}" for k, v in dt.most_common()))

    # ---- teleports, wards, deaths, spread ----
    md += ["", "## Teleports, wards, deaths", "", "| hero | teleports / game | median gems when teleporting | median hp | wards near altar / elsewhere | deaths / game |",
           "|---|---|---|---|---|---|"]
    for hero in heroes:
        tps = [t for r in results for t in r["teleports"].get(hero, [])]
        wd = Counter()
        for r in results:
            wd.update(r["wards"].get(hero, {}))
        n = sum(1 for r in results if hero in r["activity"])
        deaths = sum(r["deaths"].get(hero, 0) for r in results)
        md.append(f"| {hero} | {len(tps) / max(n, 1):.2f} | {np.median([t['gems'] for t in tps]) if tps else float('nan'):.0f} | "
                  f"{np.median([t['hp'] for t in tps]) if tps else float('nan'):.2f} | {wd.get('near altar', 0)} / {wd.get('elsewhere', 0)} | {deaths / max(n, 1):.2f} |")
    spread = defaultdict(list)
    for r in results:
        for b, v in r["spread"].items():
            spread[int(b)].extend(v)
    if spread:
        fig, ax = plt.subplots(figsize=(7, 3.2))
        bs = sorted(spread)
        ax.plot([b * BUCKET for b in bs], [np.median(spread[b]) for b in bs], "o-", color="#4C78A8")
        ax.fill_between([b * BUCKET for b in bs], [np.percentile(spread[b], 25) for b in bs],
                        [np.percentile(spread[b], 75) for b in bs], alpha=.25, color="#4C78A8")
        ax.set_xlabel("round"); ax.set_ylabel("mean pairwise distance (tiles)"); ax.set_title("How spread out the team is")
        fig.tight_layout(); fig.savefig(OUT / "spread.png", dpi=110); plt.close(fig)
        md += ["", "![spread](spread.png)"]

    (OUT / "report.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"wrote {OUT / 'report.md'}")


if __name__ == "__main__":
    main()
