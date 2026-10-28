"""Check the C++ feature port against features.py on a real game.

1. Build a bot with pdi::Policy and `policy.dump_features = true`, play a local match
   (`cambc run ...`) and save its stdout (lines "PDIF <round> <champion> f0,f1,...").
2. python training/compare_features.py --log run.log --replay replays/<match>.zip --team 0

The replay is re-simulated with the same extractor used for training, features.py computes the
vector for every (round, champion) turn, and each feature is compared with what the C++ printed.
"""
import argparse
import gzip
import json
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402

TRAINING = Path(__file__).resolve().parent


def python_features(replay, team):
    out = Path(tempfile.mkdtemp()) / "game.jsonl.gz"
    subprocess.run(["node", str(TRAINING / "extract" / "extract_game.mjs"), str(replay), str(team), str(out)], check=True)
    lines = gzip.open(out, "rt", encoding="utf-8").read().splitlines()
    header = json.loads(lines[0])["header"]
    ms = F.MapStatic(header)
    mems, feats = {}, {}
    for line in lines[1:]:
        r = json.loads(line)
        mem = mems.setdefault(r["c"], F.Memory(ms))
        feats[(r["r"], r["c"])] = np.array(F.turn_features(ms, mem, r), dtype=np.float32)
    return feats


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--log", required=True)
    p.add_argument("--replay", required=True)
    p.add_argument("--team", type=int, required=True, help="team index (0 = side a) the PDI-clone bot played")
    p.add_argument("--tol", type=float, default=1e-3)
    args = p.parse_args()

    cpp = {}
    for line in open(args.log, encoding="utf-8", errors="replace"):
        if line.startswith("PDIF "):
            _, rnd, champ, vals = line.split(maxsplit=3)
            cpp[(int(rnd), int(champ))] = np.array([float(v) for v in vals.split(",")], dtype=np.float32)
    if not cpp:
        sys.exit("no PDIF lines in the log (set policy.dump_features = true)")
    py = python_features(args.replay, args.team)
    common = sorted(set(cpp) & set(py))
    print(f"turns: c++ {len(cpp)}, python {len(py)}, matched {len(common)}")
    bad = Counter()
    example = {}
    for key in common:
        a, b = cpp[key], py[key]
        if len(a) != len(b):
            sys.exit(f"feature count differs: c++ {len(a)} vs python {len(b)} (rebuild/re-export?)")
        both_nan = np.isnan(a) & np.isnan(b)
        diff = ~both_nan & ~(np.abs(a - b) <= args.tol * np.maximum(1, np.abs(b)))
        for i in np.nonzero(diff)[0]:
            bad[F.NAMES[i]] += 1
            example.setdefault(F.NAMES[i], (key, float(a[i]), float(b[i])))
    if not bad:
        print("OK: every feature matches")
        return
    print(f"{len(bad)} features differ:")
    for name, n in bad.most_common():
        key, a, b = example[name]
        print(f"  {name:<24} {n:6d} turns ({n / len(common):.1%})  e.g. round {key[0]} champ {key[1]}: c++ {a} vs python {b}")
    sys.exit(1)


if __name__ == "__main__":
    main()
