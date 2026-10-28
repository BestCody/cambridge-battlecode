"""Turn records -> feature matrices, one .npz per game (parallel, resumable, version-checked).

    python training/build_dataset.py                 # from analysis/print_dont_int
    python training/build_dataset.py --workers 12 --limit 20

Reads  training/data/raw/<matchId>.jsonl.gz   (from extract/run_extract.py)
Writes training/data/features/<matchId>.npz   (X, hero, round, y_<head>, X_shop, y_shop, hero_shop)
       training/data/feature_spec.json        (feature names, heads, classes, version)
A file built with an older FEATURE_VERSION is rebuilt automatically.
"""
import argparse
import gzip
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np
from tqdm import tqdm

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402

TRAINING = Path(__file__).resolve().parent
RAW, OUT = TRAINING / "data" / "raw", TRAINING / "data" / "features"
MIN_TURNS = 200   # games that desynced at the very start have (almost) nothing in them


def build(raw_path, out_path):
    lines = gzip.open(raw_path, "rt", encoding="utf-8").read().splitlines()
    head = json.loads(lines[0])
    records = [json.loads(l) for l in lines[1:]]
    if len(records) < MIN_TURNS:
        return 0
    arrays = F.game_arrays(head["header"], records)
    tmp = out_path.with_name(out_path.stem + ".part.npz")
    np.savez_compressed(tmp, version=np.int32(F.FEATURE_VERSION), **arrays)
    os.replace(tmp, out_path)
    return len(records)


def up_to_date(path):
    try:
        with np.load(path) as z:
            return int(z["version"]) == F.FEATURE_VERSION
    except Exception:
        return False


def write_spec():
    spec = {"version": F.FEATURE_VERSION, "features": F.NAMES, "heads": F.HEADS + ["shop"], "n_classes": F.N_CLASSES,
            "target_classes": F.TARGET_CLASSES, "dirs": F.DIRS, "offset5": F.OFFSET5, "offset7": F.OFFSET7,
            "shop_none": F.SHOP_NONE, "champions": F.CHAMPS}
    (TRAINING / "data" / "feature_spec.json").write_text(json.dumps(spec, indent=1), encoding="utf-8")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    p.add_argument("--limit", type=int, default=0)
    args = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    write_spec()
    raws = sorted(RAW.glob("*.jsonl.gz"))
    if args.limit:
        raws = raws[:args.limit]
    jobs = [(r, OUT / (r.name[:-len(".jsonl.gz")] + ".npz")) for r in raws]
    todo = [j for j in jobs if not up_to_date(j[1])]
    failed, turns = [], 0
    with ProcessPoolExecutor(args.workers) as pool, \
            tqdm(total=len(jobs), initial=len(jobs) - len(todo), desc="features", unit=" games") as bar:
        futures = {pool.submit(build, r, o): r for r, o in todo}
        for fut in as_completed(futures):
            try:
                n = fut.result()
                turns += n
                bar.set_postfix(turns=turns)
            except Exception as e:
                failed.append(futures[fut].name)
                tqdm.write(f"  failed {futures[fut].name}: {type(e).__name__}: {e}")
            bar.update(1)
    print(f"done: {len(jobs) - len(failed)} games" + (f", {len(failed)} failed" if failed else ""))


if __name__ == "__main__":
    main()
