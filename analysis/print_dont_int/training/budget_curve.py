"""How much accuracy each head keeps when truncated to its first N boosting rounds, and what that
costs the C++ bot per call (tree levels walked ~ fuel / 12). No training: it slices the saved models.

    python training/budget_curve.py --hero vesper
    python training/budget_curve.py --hero all --heads move,attack,ability,recall,cancel,ward
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402
from train import DEFAULT_CKPT, HEROES, head_rows, load_hero  # noqa: E402

ROUNDS = [5, 10, 20, 40, 80, 150, 300, 600]
FUEL_PER_LEVEL = 12   # rough wasm instructions per tree level in pdi_tree.h's loop


def max_depths(model_json):
    out = []
    for t in model_json["learner"]["gradient_booster"]["model"]["trees"]:
        L, R = t["left_children"], t["right_children"]
        stack, deepest = [(0, 0)], 0
        while stack:
            i, d = stack.pop()
            if L[i] == -1:
                deepest = max(deepest, d)
            else:
                stack += [(L[i], d + 1), (R[i], d + 1)]
        out.append(deepest)
    return out


def main():
    import xgboost as xgb

    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--hero", default="vesper")
    p.add_argument("--heads", default="move,attack,ability,recall,cancel,ward,dash,shop")
    p.add_argument("--ckpt-dir", default=str(DEFAULT_CKPT))
    args = p.parse_args()
    heroes = list(HEROES) if args.hero == "all" else args.hero.split(",")
    for hero in heroes:
        data, games = load_hero(HEROES[hero])
        print(f"\n== {hero}   (levels = tree levels walked per call; fuel ~ {FUEL_PER_LEVEL} x levels)")
        print(f"{'head':<12}" + "".join(f"{n:>16}" for n in ROUNDS))
        for head in args.heads.split(","):
            folder = Path(args.ckpt_dir) / hero / head / "best"
            if not (folder / "model.json").exists():
                continue
            state = json.loads((folder / "state.json").read_text())
            st_args = argparse.Namespace(**state["args"])
            parts = head_rows(data, games, head, st_args)
            Xva, yva = parts["val"]
            remap = {c: i for i, c in enumerate(state["classes"])}
            keep = np.isin(yva, state["classes"])
            y = np.array([remap[c] for c in yva[keep]])
            booster = xgb.Booster(model_file=str(folder / "model.json"))
            K = len(state["classes"])
            groups = 1 if K == 2 else K
            depth = max_depths(json.loads((folder / "model.json").read_text()))
            dm = xgb.DMatrix(Xva[keep], feature_names=F.NAMES, missing=np.nan)
            cells = []
            for n in ROUNDS:
                n = min(n, booster.num_boosted_rounds())
                pr = booster.predict(dm, iteration_range=(0, n))
                pred = (pr > 0.5).astype(int) if K == 2 else pr.argmax(1)
                levels = sum(depth[:n * groups])
                cells.append(f"{(pred == y).mean():.3f} @{levels:>6}")
            print(f"{head:<12}" + "".join(f"{c:>16}" for c in cells))


if __name__ == "__main__":
    main()
