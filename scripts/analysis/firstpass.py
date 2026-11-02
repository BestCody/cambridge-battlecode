"""First pass over a downloaded match summary (cambc matches summary --json).

    python firstpass.py summary.json [us-substring]

Prints the team timeline at rounds 100/300/500/800, each champion's totals,
the emblem order, and every death with its round, place and killer.
"""
import json
import sys
from collections import defaultdict

CHECK = (100, 300, 500, 800, 1200)


def at(runs, rnd):
    """Value of a run-length timeline at the start of round rnd."""
    t = 0
    val = 0
    for r in runs:
        if t + r["duration"] > rnd:
            return float(r["value"])
        t += r["duration"]
        val = float(r["value"])
    return val


def main():
    d = json.load(open(sys.argv[1], encoding="utf-8"))
    us = sys.argv[2] if len(sys.argv) > 2 else None
    names = {}
    for t in d["teams"]:
        names[t["team"]] = t.get("name") or t["team"]
    roster = {c["id"]: c for c in d["roster"]["champions"]}
    teams = sorted({c["team"] for c in roster.values()})
    print("map", d["match"].get("mapName") or d["match"].get("map"), "rounds", d["match"].get("finalRound"),
          "| final gems", {names[t["team"]]: t["submittedGems"] for t in d["teams"]})
    for t in teams:
        print(" ", t, names[t], ":", ", ".join(c["name"] for c in roster.values() if c["team"] == t))

    tl = defaultdict(dict)  # (team, champ id) -> metric -> runs
    for e in d["championTimelines"]:
        o = e["owner"]
        tl[(o["team"], o["id"])][e["metric"]] = e["runs"]
    metrics = ["earnedGold", "submittedGems", "collectedGems", "kills", "deaths", "championDamageDone", "healingDone", "spentGold"]

    print("\nteam timeline (gold / banked gems / kills / deaths)")
    print("%-6s" % "round" + "".join("%-34s" % names[t] for t in teams))
    for rnd in CHECK:
        row = "%-6d" % rnd
        for t in teams:
            tot = defaultdict(float)
            for (tt, cid), ms in tl.items():
                if tt != t:
                    continue
                for m in ("earnedGold", "submittedGems", "kills", "deaths"):
                    tot[m] += at(ms.get(m, []), rnd)
            row += "%-34s" % ("%5.0f / %4.0f / %2.0f / %2.0f" % (tot["earnedGold"], tot["submittedGems"], tot["kills"], tot["deaths"]))
        print(row)

    print("\nchampion totals")
    print("%-6s %-14s %6s %6s %6s %4s %4s %7s %7s %6s" % ("team", "champ", "gold", "banked", "coll", "K", "D", "dmg", "heal", "spent"))
    for (t, cid), ms in sorted(tl.items()):
        vals = [at(ms.get(m, []), 10 ** 6) for m in metrics]
        print("%-6s %-14s %6.0f %6.0f %6.0f %4.0f %4.0f %7.0f %7.0f %6.0f" % (t, roster[cid]["name"], *vals))

    ev = defaultdict(dict)  # kind -> (team, id) -> entries
    for e in d["events"]:
        o = e["owner"]
        ev[e["kind"]][(o["team"], o["id"])] = e["entries"]

    print("\nemblems (round: item)")
    for (t, cid), entries in sorted(ev.get("itemCombination", {}).items()):
        items = ["r%d %s" % (x["timestamp"]["round"], x["target"]["name"].replace(" Emblem of ", "/")) for x in entries]
        print("  %s %-14s %s" % (t, roster[cid]["name"], "; ".join(items)))

    print("\ndeaths (round place <- killer)")
    killers = {}
    for (t, cid), entries in ev.get("kill", {}).items():
        for x in entries:
            killers[(x["target"]["team"], x["target"]["id"], x["timestamp"]["round"])] = roster[cid]["name"]
    rows = []
    for (t, cid), entries in ev.get("death", {}).items():
        for x in entries:
            r = x["timestamp"]["round"]
            k = killers.get((t, cid, r)) or killers.get((t, cid, r - 1)) or "?"
            rows.append((r, t, roster[cid]["name"], x["x"], x["y"], k))
    for r, t, n, x, y, k in sorted(rows):
        print("  r%-5d %s %-14s (%2d,%2d) <- %s" % (r, t, n, x, y, k))

    print("\nbanking (round: gems)")
    for (t, cid), entries in sorted(ev.get("gemSubmission", {}).items()):
        if entries:
            print("  %s %-14s %s" % (t, roster[cid]["name"], " ".join("r%d:%s" % (x["timestamp"]["round"], x["value"]) for x in entries)))


if __name__ == "__main__":
    main()
