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
value in the bot's strategy.h. Each bot is built once; matches run through `cambc run-batch` in release mode
(as on the platform), on at most 90% of the cores at below-normal priority.
Patched configs use temporary copies under bots/_eval_*, removed afterwards.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FUELS = [92000, 96000, 100000, 104000, 108000]


def patch(text, name, value):
    pattern = rf"(inline constexpr [\w:]+ {re.escape(name)}\s*=\s*)[^;]+;"
    if not re.search(pattern, text):
        raise ValueError(f"constant not found in strategy.h: {name}")
    return re.sub(pattern, rf"\g<1>{value};", text)


# Leave the machine usable: at most 90% of cores, and every cambc process at
# below-normal priority so the user's own programs always come first.
JOBS = max(1, int((os.cpu_count() or 2) * 0.9))
LOW_PRIORITY = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {"preexec_fn": lambda: os.nice(10)}


def cambc(*command, check=True):
    done = subprocess.run(["cambc", *command], cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", **LOW_PRIORITY)
    if check and done.returncode:
        raise RuntimeError(f"cambc {' '.join(command[:2])} failed:\n{done.stdout[-1500:]}{done.stderr[-1500:]}")
    return done


def prepare_bot(name, source, assignments):
    """A bot folder to play: the source itself, or a patched copy. Built once."""
    if not assignments:
        cambc("build", source)
        return source
    target = ROOT / "bots" / f"_eval_{name}"
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(ROOT / "bots" / source, target, ignore=shutil.ignore_patterns(".cambc", "compile_commands.json"))
    strategy = target / "src" / "strategy.h"
    text = strategy.read_text()
    for constant, value in assignments:
        text = patch(text, constant, value)
    strategy.write_text(text)
    cambc("build", target.name)
    return target.name


def play(configs, opponents, fuels, args):
    """Run every match through cambc run-batch, which builds once and parallelises."""
    out = Path(args.out)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    bots, temporary = {}, []
    try:
        for name, source, assignments in configs:
            for opponent in opponents:
                key = (name, opponent)
                bots[key] = prepare_bot(name, source or opponent, assignments)
                if assignments:
                    temporary.append(bots[key])
        for opponent in opponents:
            cambc("build", opponent)
        games = []
        for fuel in fuels:
            rows, expected = [], []
            for name, _, _ in configs:
                for opponent in opponents:
                    for game_map in args.maps:
                        for seat in ("A", "B"):
                            a, b = (bots[(name, opponent)], opponent) if seat == "A" else (opponent, bots[(name, opponent)])
                            tag = f"{name}-{opponent}-{fuel}-{seat}-{game_map}"
                            stats, replay = out / f"{tag}.json", out / f"{tag}.repl26a"
                            # run-batch resolves relative paths from the manifest's folder.
                            bot_a, bot_b = (ROOT / "bots" / a).as_posix(), (ROOT / "bots" / b).as_posix()
                            map_path = (ROOT / "maps" / f"{game_map}.map26a").as_posix()
                            rows.append(f"{bot_a} {bot_b} {map_path} {replay.as_posix()} --statistics {stats.as_posix()} --json")
                            expected.append((name, opponent, game_map, seat, stats))
            manifest = out / f"manifest-{fuel}.txt"
            manifest.write_text("\n".join(rows) + "\n")
            print(f"  running {len(rows)} matches at fuel {fuel} on {args.jobs} processes...", flush=True)
            cambc("run-batch", str(manifest), "--jobs", str(args.jobs), "--fuel-quantum", str(fuel), "--release",
                  "--json", "--color", "never", check=False)
            for name, opponent, game_map, seat, stats in expected:
                if not stats.exists():
                    print(f"  missing result: {stats.name}", flush=True)
                    continue
                result = json.loads(stats.read_text(encoding="utf-8"))
                ours = 0 if seat == "A" else 1
                teams = result["teams"]
                games.append({
                    "config": name, "opponent": opponent, "fuel": fuel, "map": game_map, "seat": seat,
                    "won": result["match"]["outcome"] == ("teamA" if ours == 0 else "teamB"),
                    "ours": int(teams[ours]["submittedGems"]),
                    "theirs": int(teams[1 - ours]["submittedGems"]),
                    "round": result["match"]["roundsCompleted"],
                })
        return games
    finally:
        for name in temporary:
            shutil.rmtree(ROOT / "bots" / name, ignore_errors=True)


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
    parser.add_argument("--jobs", type=int, default=JOBS, help="parallel matches (default: 90%% of cores)")
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

    total = len(configs) * len(opponents) * len(fuels) * len(args.maps) * 2
    print(f"{len(configs)} configs x {len(opponents)} opponents x {len(fuels)} fuel budgets x {len(args.maps)} maps "
          f"x 2 seats = {total} games, up to {args.jobs} processes at below-normal priority", flush=True)
    games = play(configs, opponents, fuels, args)
    Path(args.out).mkdir(parents=True, exist_ok=True)
    (Path(args.out) / "games.json").write_text(json.dumps(games, indent=1))
    report(games, names, opponents)


if __name__ == "__main__":
    main()
