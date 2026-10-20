"""Decide whether a bot change beats the baseline.

cambc matches are deterministic but chaotic: a one-line change can flip a
third of single games. One game per map is therefore not evidence. This script
plays every map in both seats across several fuel budgets near the real 100k
limit (budget changes shift when searches defer, producing genuinely different
games) and reports wins and gems for both sides against each opponent.

Run from any directory:
    python scripts/evaluate.py                          # newest-bot vs baseline
    python scripts/evaluate.py --mirror                 # add baseline-vs-baseline noise floor
    python scripts/evaluate.py off:hidden_interference_weight=0.0f current
    python scripts/evaluate.py --bot my-bot --baseline baseline --maps eight,swirl
    python scripts/evaluate.py --baseline db11-looter,db12-comeback --fuels 100000

A config is NAME or NAME:constant=value,... where each constant is a policy
value in the bot's strategy.h. Every (config, opponent, fuel) triple runs in its
own process with private copies of both bots under bots/_eval_*, removed
afterwards.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FUELS = [92000, 96000, 100000, 104000, 108000]


def patch(text, name, value):
    pattern = rf"(inline constexpr [\w:]+ {re.escape(name)}\s*=\s*)[^;]+;"
    if not re.search(pattern, text):
        raise ValueError(f"constant not found in strategy.h: {name}")
    return re.sub(pattern, rf"\g<1>{value};", text)


def copy_bot(source, name):
    target = ROOT / "bots" / name
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(ROOT / "bots" / source, target,
                    ignore=shutil.ignore_patterns(".cambc", "compile_commands.json"))
    return target


def run_task(config, opponent, fuel, args):
    """Play every map in both seats at one fuel budget. Returns a list of games."""
    name, source, assignments = config
    source = source or opponent  # The mirror config plays the opponent against itself.
    tag = f"{name}_{opponent}_{fuel}"
    new, base = f"_eval_{tag}", f"_eval_{tag}_base"
    out = Path(args.out) / tag
    out.mkdir(parents=True, exist_ok=True)
    try:
        strategy = copy_bot(source, new) / "src" / "strategy.h"
        copy_bot(opponent, base)
        if assignments:
            text = strategy.read_text()
            for constant, value in assignments:
                text = patch(text, constant, value)
            strategy.write_text(text)
        for bot in (new, base):
            built = subprocess.run(["cambc", "build", bot], cwd=ROOT, capture_output=True, text=True)
            if built.returncode:
                raise RuntimeError(f"build failed for {bot}:\n{built.stdout[-800:]}{built.stderr[-800:]}")
        games = []
        for game_map in args.maps:
            for seat, (a, b) in (("A", (new, base)), ("B", (base, new))):
                stats = out / f"{seat}-{game_map}.json"
                subprocess.run(["cambc", "run", a, b, "--map", game_map, "--fuel", str(fuel),
                                "--statistics", str(stats), "-o", str(out / "replays" / f"{seat}-{game_map}")],
                               cwd=ROOT, capture_output=True)
                result = json.loads(stats.read_text())
                ours = 0 if seat == "A" else 1
                games.append({
                    "config": name, "opponent": opponent, "fuel": fuel, "map": game_map, "seat": seat,
                    "won": (result["outcome"] == "teamA") == (ours == 0),
                    "ours": result["teams"][ours]["submittedGems"],
                    "theirs": result["teams"][1 - ours]["submittedGems"],
                    "round": result["round"],
                })
        return games
    finally:
        for bot in (new, base):
            shutil.rmtree(ROOT / "bots" / bot, ignore_errors=True)


def parse_config(spec, bot):
    name, _, rest = spec.partition(":")
    assignments = [tuple(a.split("=", 1)) for a in rest.split(",") if a]
    return name, bot, assignments


def short(game_map):
    return game_map.split("-")[0] if re.search(r"-[0-9a-f]{8}-", game_map) else game_map


def report(games, configs, opponents):
    pairs = [(name, opponent) for opponent in opponents for name in configs]
    print()
    print(f"{'config':14} {'opponent':16} {'wins':>10}  {'gems ours':>10} {'gems them':>10} {'margin':>8}")
    for name, opponent in pairs:
        rows = [g for g in games if g["config"] == name and g["opponent"] == opponent]
        if not rows:
            continue
        wins = sum(g["won"] for g in rows)
        ours, theirs = sum(g["ours"] for g in rows), sum(g["theirs"] for g in rows)
        print(f"{name:14} {opponent:16} {wins:4}/{len(rows):<4} {wins / len(rows):4.0%}  "
              f"{ours:10} {theirs:10} {ours - theirs:+8}")
    for name, opponent in pairs:
        rows = [g for g in games if g["config"] == name and g["opponent"] == opponent]
        if not rows:
            continue
        print(f"\n{name} vs {opponent}: per map (A/B per fuel budget, ascending)")
        by_map = defaultdict(list)
        for g in sorted(rows, key=lambda g: (g["fuel"], g["seat"])):
            by_map[g["map"]].append(g)
        for game_map, played in sorted(by_map.items(), key=lambda kv: sum(g["won"] for g in kv[1]) / len(kv[1])):
            wins = sum(g["won"] for g in played)
            pattern = "".join("W" if g["won"] else "L" for g in played)
            print(f"  {short(game_map):12} {wins:2}/{len(played):<2} gems {sum(g['ours'] for g in played):6} vs "
                  f"{sum(g['theirs'] for g in played):6}  {pattern}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("configs", nargs="*", default=["current"], help="NAME or NAME:const=value,...")
    parser.add_argument("--bot", default="newest-bot", help="bot folder under bots/ to evaluate")
    parser.add_argument("--baseline", default="baseline",
                        help="comma-separated bot folders under bots/ to play against")
    parser.add_argument("--maps", default="", help="comma-separated map names (default: whole pool)")
    parser.add_argument("--fuels", default=",".join(map(str, DEFAULT_FUELS)), help="comma-separated fuel budgets")
    parser.add_argument("--mirror", action="store_true", help="also play each opponent against itself")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2), help="parallel processes")
    parser.add_argument("--out", default=str(Path(os.environ.get("TEMP", "/tmp")) / "evaluate"),
                        help="directory for statistics, replays and games.json")
    args = parser.parse_args()

    pool_maps = sorted(p.stem for p in (ROOT / "maps").glob("*.map26a"))
    wanted = [m for m in args.maps.split(",") if m]
    args.maps = [m for m in pool_maps if not wanted or short(m) in wanted or m in wanted]
    fuels = [int(f) for f in args.fuels.split(",")]
    opponents = [b for b in args.baseline.split(",") if b]
    configs = [parse_config(spec, args.bot) for spec in args.configs]
    if args.mirror:
        configs.insert(0, ("mirror", None, []))
    names = [c[0] for c in configs]
    if len(set(names)) != len(names):
        sys.exit("config names must be unique")
    for bot in [source for _, source, _ in configs if source] + opponents:
        if not (ROOT / "bots" / bot).is_dir():
            sys.exit(f"no such bot: bots/{bot}")

    tasks = [(config, opponent, fuel) for config in configs for opponent in opponents for fuel in fuels]
    total = len(tasks) * len(args.maps) * 2
    print(f"{len(configs)} configs x {len(opponents)} opponents x {len(fuels)} fuel budgets x {len(args.maps)} maps "
          f"x 2 seats = {total} games, {min(args.jobs, len(tasks))} processes", flush=True)
    games = []
    with ProcessPoolExecutor(max_workers=min(args.jobs, len(tasks))) as pool:
        futures = {pool.submit(run_task, config, opponent, fuel, args): (config[0], opponent, fuel)
                   for config, opponent, fuel in tasks}
        for future in as_completed(futures):
            name, opponent, fuel = futures[future]
            played = future.result()
            games.extend(played)
            print(f"  done {name} vs {opponent} @ {fuel}: {sum(g['won'] for g in played)}/{len(played)}", flush=True)
    Path(args.out).mkdir(parents=True, exist_ok=True)
    (Path(args.out) / "games.json").write_text(json.dumps(games, indent=1))
    report(games, names, opponents)


if __name__ == "__main__":
    main()
