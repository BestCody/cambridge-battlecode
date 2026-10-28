"""Extract every downloaded replay into per-turn observation/action records, in parallel and resumably.

    python training/extract/run_extract.py               # from analysis/print_dont_int
    python training/extract/run_extract.py --workers 12 --limit 50

Writes training/data/raw/<matchId>.jsonl.gz (one per game, skipped when present) and
training/data/games.csv (one row per game: map, side, result, opponent, picks, ...).
Needs node (>= 18) and the cambc visualiser's engine 2.0.0 wasm (found automatically).
"""
import argparse
import csv
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from tqdm import tqdm

HERE = Path(__file__).resolve().parent
TRAINING = HERE.parent
TEAM_DIR = TRAINING.parent
TEAM_ID = "d559d741-ac53-411e-bc4e-36b8f3d12d5a"


def game_rows(index, replays):
    rows = []
    for zip_path in sorted(replays.glob("*.zip")):
        m = index.get(zip_path.stem)
        if not m or m.get("status") != "finished":
            continue
        us = next((t for t in m["teams"] if t["teamId"] == TEAM_ID), None)
        them = next((t for t in m["teams"] if t["teamId"] != TEAM_ID), None)
        if us is None or them is None:
            continue
        rows.append({
            "matchId": m["matchId"], "createdAt": m["createdAt"], "kind": m["kind"], "map": m["map"],
            "team": 0 if us["side"] == "a" else 1, "result": us["result"], "endReason": m.get("endReason"),
            "opponent": them["name"], "picks": "-".join(map(str, us["draftPicks"])),
            "opponentPicks": "-".join(map(str, them["draftPicks"])), "replay": str(zip_path),
        })
    return rows


def extract(row, out_dir):
    out = out_dir / f"{row['matchId']}.jsonl.gz"
    proc = subprocess.run(["node", str(HERE / "extract_game.mjs"), row["replay"], str(row["team"]), str(out), row["matchId"]],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        lines = (proc.stderr or proc.stdout).strip().splitlines()
        errors = [l for l in lines if l.lstrip().startswith(("Error", "RangeError", "TypeError"))]
        raise RuntimeError((errors or lines or [f"exit {proc.returncode}"])[0][:200])
    return json.loads(proc.stdout.strip().splitlines()[-1])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--team-dir", default=str(TEAM_DIR), help="folder holding index.json and replays/")
    p.add_argument("--workers", type=int, default=8)
    p.add_argument("--limit", type=int, default=0, help="only the first N games (0 = all)")
    args = p.parse_args()

    team_dir = Path(args.team_dir)
    index = json.loads((team_dir / "index.json").read_text(encoding="utf-8"))
    rows = game_rows(index, team_dir / "replays")
    if args.limit:
        rows = rows[:args.limit]
    data = TRAINING / "data"
    raw = data / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    with open(data / "games.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    todo = [r for r in rows if not (raw / f"{r['matchId']}.jsonl.gz").exists()]
    failed = []
    with ThreadPoolExecutor(args.workers) as pool, \
            tqdm(total=len(rows), initial=len(rows) - len(todo), desc="extracting", unit=" games") as bar:
        futures = {pool.submit(extract, r, raw): r for r in todo}
        for fut in as_completed(futures):
            r = futures[fut]
            try:
                res = fut.result()
                bar.set_postfix(turns=res["turns"], round=res["round"])
            except Exception as e:  # keep going; a rerun retries failures
                failed.append(r["matchId"])
                tqdm.write(f"  failed {r['matchId']}: {e}")
            bar.update(1)
    (raw / "failed.txt").write_text("\n".join(failed), encoding="utf-8")
    print(f"done: {len(rows) - len(failed)} games extracted" + (f", {len(failed)} failed (rerun to retry)" if failed else ""))


if __name__ == "__main__":
    sys.exit(main())
