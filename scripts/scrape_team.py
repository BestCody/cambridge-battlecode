"""Download every match another team has played, resumably.

    python scripts/scrape_team.py "print dont int"
    python scripts/scrape_team.py d559d741-ac53-411e-bc4e-36b8f3d12d5a --kind ranked

Writes under analysis/<team>/ (override with --out):
    index.json          every listed match, keyed by matchId (metadata from `cambc matches list`)
    state.json          listing cursor, so an interrupted listing carries on where it stopped
    replays/<id>.zip    replay bundle (contains replay.repl26a; open with `cambc view`)
    summaries/<id>.bin  raw match summary; convert with `cambc summary <file>`

Safe to stop (Ctrl+C) and rerun at any time: finished files are written atomically and skipped
on the next run, and a rerun also picks up matches the team has played since.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from tqdm import tqdm

ROOT = Path(__file__).resolve().parent.parent
PAGE = 100


def find_cambc():
    found = shutil.which("cambc")
    if found:
        return found
    fallback = Path(os.environ.get("APPDATA", "")) / "Python" / "Python314" / "Scripts" / "cambc.exe"
    if fallback.exists():
        return str(fallback)
    sys.exit("cambc not found on PATH")


CAMBC = find_cambc()
sys.stdout.reconfigure(errors="replace")
sys.stderr.reconfigure(errors="replace")


def last_line(text):
    lines = str(text).strip().splitlines()
    return lines[-1][:120] if lines else "(no message)"


def cambc(*args, attempts=8):
    """Run cambc, backing off on rate limits and transient failures. Returns stdout."""
    delay = 5.0
    for attempt in range(attempts):
        proc = subprocess.run([CAMBC, *args], capture_output=True, text=True, encoding="utf-8",
                              errors="replace")
        if proc.returncode == 0:
            return proc.stdout
        err = (proc.stderr or proc.stdout).strip()
        if "login" in err.lower() and "429" not in err:
            sys.exit(f"cambc needs a fresh login (run `cambc login`):\n{err}")
        retry = re.search(r"retry after (\d+)", err)
        wait = float(retry.group(1)) + 1 if retry else delay
        if attempt == attempts - 1:
            raise RuntimeError(err)
        tqdm.write(f"  cambc {args[0]} {args[1]}: {last_line(err)} (waiting {wait:.0f}s)")
        time.sleep(wait)
        delay = min(delay * 2, 300)


def write_json(path, data):
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=1), encoding="utf-8")
    for _ in range(20):  # Windows: the editor or antivirus can briefly hold the old file open
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.25)
    os.replace(tmp, path)


def list_page(team, kind, cursor):
    args = ["matches", "list", "--team", team, "--limit", str(PAGE), "--json"]
    if kind:
        args += ["--kind", kind]
    if cursor:
        args += ["--cursor", cursor]
    page = json.loads(cambc(*args))
    return page["matches"], page.get("nextCursor")


def refresh_index(team, kind, out, index, state, limit):
    """Bring index.json up to date: new matches at the head, then any unfinished tail listing."""
    def merge(matches):
        fresh = 0
        for m in matches:
            old = index.get(m["matchId"])
            if old is None or old.get("status") != m.get("status"):
                fresh += 1
            index[m["matchId"]] = m
        return fresh

    first_run = not index
    with tqdm(desc="listing", unit=" matches") as bar:
        # Head: newest first, until a page holds nothing new since the previous run.
        cursor = None
        while True:
            matches, cursor = list_page(team, kind, cursor)
            fresh = merge(matches)
            bar.update(len(matches))
            if first_run:
                state["cursor"] = cursor
            if not cursor:
                state["complete"] = True
            write_json(out / "index.json", index)
            write_json(out / "state.json", state)
            if not cursor or (not first_run and fresh == 0) or enough(index, limit):
                break

        # Tail: carry on an earlier listing that was interrupted before reaching the oldest match.
        cursor = state.get("cursor")
        while not state.get("complete") and cursor and not enough(index, limit):
            matches, cursor = list_page(team, kind, cursor)
            merge(matches)
            bar.update(len(matches))
            state["cursor"] = cursor
            state["complete"] = cursor is None
            write_json(out / "index.json", index)
            write_json(out / "state.json", state)


def enough(index, limit):
    return sum(m.get("status") == "finished" for m in index.values()) >= limit


def download(kind, match_id, target):
    tmp = target.with_name(target.name + ".part")
    tmp.unlink(missing_ok=True)
    if kind == "replay":
        cambc("matches", "download", match_id, "-o", str(tmp))
    else:
        cambc("matches", "summary", match_id, "-o", str(tmp))
    os.replace(tmp, target)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("team", help="team UUID or full team name")
    p.add_argument("--kind", choices=["ranked", "unranked", "tournament"], help="only this match kind")
    p.add_argument("--out", help="output folder (default analysis/<team name>)")
    p.add_argument("--no-summaries", action="store_true", help="replays only")
    p.add_argument("--list-only", action="store_true", help="update index.json without downloading")
    p.add_argument("--max", type=int, default=500, help="newest N finished matches (default 500)")
    p.add_argument("--pause", type=float, default=0.5, help="seconds between requests (default 0.5)")
    args = p.parse_args()

    slug = re.sub(r"[^a-z0-9]+", "_", args.team.lower()).strip("_")
    out = Path(args.out) if args.out else ROOT / "analysis" / slug
    (out / "replays").mkdir(parents=True, exist_ok=True)
    (out / "summaries").mkdir(parents=True, exist_ok=True)
    index = json.loads((out / "index.json").read_text(encoding="utf-8")) if (out / "index.json").exists() else {}
    state = json.loads((out / "state.json").read_text(encoding="utf-8")) if (out / "state.json").exists() else {}

    refresh_index(args.team, args.kind, out, index, state, args.max)
    finished = [m for m in index.values() if m.get("status") == "finished"]
    finished = sorted(finished, key=lambda m: m["createdAt"], reverse=True)[:args.max]
    print(f"{len(index)} matches listed, downloading the newest {len(finished)} finished")
    if args.list_only:
        return

    jobs = [("replay", m["matchId"], out / "replays" / f"{m['matchId']}.zip") for m in finished]
    if not args.no_summaries:
        jobs += [("summary", m["matchId"], out / "summaries" / f"{m['matchId']}.bin") for m in finished]
    todo = [j for j in jobs if not j[2].exists()]
    failed = []
    for kind, match_id, target in tqdm(todo, desc="downloading", unit=" files",
                                       initial=len(jobs) - len(todo), total=len(jobs)):
        try:
            download(kind, match_id, target)
        except (RuntimeError, OSError) as e:
            failed.append(match_id)
            tqdm.write(f"  gave up on {kind} {match_id}: {last_line(e)}")
        time.sleep(args.pause)
    print(f"done; {len(failed)} failed (rerun to retry)" if failed else "done")


if __name__ == "__main__":
    main()
