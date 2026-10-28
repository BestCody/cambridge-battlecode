"""Check that features.py computes path distances exactly like the SDK's distance_field (which is what
the C++ bot will use), on every distinct map in the dataset (or the --n most walled ones).

    python training/check_parity.py --n 8
"""
import argparse
import csv
import gzip
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402

TRAINING = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--n", type=int, default=0, help="only the N maps with the most walls+holes (0 = all)")
    args = p.parse_args()
    games = {r["matchId"]: r["map"] for r in csv.DictReader(open(TRAINING / "data" / "games.csv", encoding="utf-8"))}
    maps = {}
    for f in sorted((TRAINING / "data" / "raw").glob("*.jsonl.gz")):
        name = games.get(f.name[:-len(".jsonl.gz")])
        if name in maps:
            continue
        header = json.loads(gzip.open(f, "rt", encoding="utf-8").readline())["header"]
        maps[name] = (f, header, sum(r.count("1") for r in header["walls"] + header["holes"]))
    chosen = sorted(maps.items(), key=lambda kv: -kv[1][2])
    if args.n:
        chosen = chosen[:args.n]
    out = Path(tempfile.mkdtemp()) / "d.json"
    bad_maps = 0
    for name, (f, header, blocked) in chosen:
        subprocess.run(["node", str(TRAINING / "extract" / "sdk_distance.mjs"), str(f), str(out)], check=True,
                       capture_output=True)
        d = json.loads(out.read_text())
        ms = F.MapStatic(header)
        pairs = [("own", ms.d_own), ("enemy", ms.d_enemy), ("altar", ms.d_altar)]
        pairs += [(f"group{i}", ms.d_group[i]) for i in range(len(ms.groups))]
        bad = 0
        for key, mine in pairs:
            sdk = np.array(d["fields"][key], dtype=np.float32).reshape(d["H"], d["W"])
            sdk[sdk >= 65535] = F.FAR
            bad += int((sdk != mine).sum())
        bad_maps += bad > 0
        print(f"{name:<28} {d['W']}x{d['H']} blocked {blocked:5d} fields {len(pairs):3d}  mismatched tiles {bad}")
    print("OK: python BFS == SDK distance_field" if not bad_maps else f"MISMATCH on {bad_maps} maps")
    sys.exit(1 if bad_maps else 0)


if __name__ == "__main__":
    main()
