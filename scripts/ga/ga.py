"""Genetic algorithm over one bot's policy:: constants, on the maps that bot is
picked for.

Each individual is a set of constant values written into a copy of the bot's
strategy.h. A generation plays every individual, plus the current values as a
permanent control, on the same freshly sampled maps (both seats, against each
opponent), so individuals are compared on identical games. Fitness per game:
0.5 * win (draw 0.5) + 0.5 * our share of the submitted gems; a bot fault
counts as a loss with no gems. Elites carry over, parents come from 3-way
tournaments, children take each gene from either parent or a blend, and
mutation shrinks from 30% to 10% of a gene's range over the generations.

Runs anywhere `cambc` (4.0.0) is on PATH; run it from the repository root (the
folder with cambc.toml). Games run through `cambc run-batch`; results come from
each game's statistics file, which is deleted with its replay once read.

  python scripts/ga/ga.py --bot baseline --params scripts/ga/params_baseline.json \
      --maps scripts/ga/maps_sparse.txt --opponents bots/test-bot,bots/baseline \
      --out ga_runs/baseline --jobs 190
  python scripts/ga/ga.py ... --resume                         (continue a run)
  python scripts/ga/ga.py --validate ga_runs/baseline/best.json ... --maps <list>
"""
import argparse, json, math, os, random, re, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


# ------------------------------------------------------------------ genes
def load_params(path):
    specs = json.load(open(path, encoding="utf-8"))
    for s in specs:
        assert s["type"] in ("float", "int", "unsigned"), s
        assert s["lo"] <= s["default"] <= s["hi"], s
    return specs


def clamp(spec, v):
    v = min(spec["hi"], max(spec["lo"], v))
    return round(v) if spec["type"] != "float" else v


def literal(spec, v):
    # A C++ float literal needs a point or an exponent before its f.
    if spec["type"] == "float":
        text = f"{v:.5g}"
        return text + ("f" if "e" in text or "." in text else ".0f")
    return str(int(round(v)))


def write_strategy(src_dir, specs, values):
    path = os.path.join(src_dir, "strategy.h")
    text = open(path, "rb").read().decode("utf-8")
    for spec in specs:
        pattern = re.compile(r"(inline constexpr [\w:]+ " + re.escape(spec["name"]) + r" = )([^;]+)(;)")
        found = pattern.findall(text)
        if len(found) != 1:
            sys.exit(f"{spec['name']}: {len(found)} definitions in {path}")
        text = pattern.sub(lambda m: m.group(1) + literal(spec, values[spec["name"]]) + m.group(3), text)
    open(path, "wb").write(text.encode("utf-8"))


# ------------------------------------------------------------------ bots and games
def make_bot(bot_src, specs, values, folder):
    # A bot folder per individual: run-batch keys builds by folder name.
    if os.path.exists(folder):
        shutil.rmtree(folder)
    shutil.copytree(bot_src, os.path.join(folder, "src"), ignore=shutil.ignore_patterns(".cambc"))
    write_strategy(os.path.join(folder, "src"), specs, values)


def build(folder):
    done = subprocess.run(["cambc", "build", folder, "--release"], cwd=REPO, capture_output=True, text=True,
                          errors="replace")
    if done.returncode != 0:
        tail = (done.stdout + done.stderr).strip().splitlines()[-15:]
        raise RuntimeError(f"build failed: {folder}\n" + "\n".join(tail))


def build_all(folders, workers):
    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        for future in [pool.submit(build, f) for f in folders]:
            future.result()


def read_result(stats_path, our_team):
    """(fitness, won, gem share, fault) from a statistics file, or None."""
    try:
        d = json.load(open(stats_path, encoding="utf-8"))
    except (OSError, ValueError):
        return None
    them = "teamB" if our_team == "teamA" else "teamA"
    gems = {t["team"]: int(t["submittedGems"]) for t in d.get("teams", [])}
    faults = d.get("diagnostics", {}).get("botFaults", {})
    outcome = d.get("match", {}).get("outcome")
    fault = int(faults.get(our_team, 0)) > 0
    total = gems.get(our_team, 0) + gems.get(them, 0)
    share = gems.get(our_team, 0) / total if total else 0.5
    won = 1.0 if outcome == our_team else 0.0 if outcome == them else 0.5
    if fault:
        won, share = 0.0, 0.0
    return 0.5 * won + 0.5 * share, won, share, fault


def play_values(bot_src, specs, values_list, games, scratch, tag, jobs, build_workers, fuel, timeout):
    """Builds the value sets the games use and plays them. games: list of
    (key, value index, opponent, map, seat). Returns {key: result}. Each call
    runs on one machine; PLAY may split the games across several."""
    folders = {}
    for vi in sorted({g[1] for g in games}):
        folders[vi] = os.path.join(scratch, "bots", f"{os.path.basename(os.path.dirname(bot_src))}_{tag}_i{vi:02d}")
        make_bot(bot_src, specs, values_list[vi], folders[vi])
    opponents = sorted({g[2] for g in games})
    build_all(list(folders.values()) + opponents, build_workers)
    results = play([(key, folders[vi], opponent, m, seat) for key, vi, opponent, m, seat in games],
                   os.path.join(scratch, tag), jobs, fuel, timeout)
    for folder in folders.values():
        shutil.rmtree(folder, ignore_errors=True)  # Individuals are rebuilt from values when needed.
    return results


def play_here(args, specs, values_list, games, tag):
    return play_values(args.bot_src, specs, values_list, games, args.out, tag, args.jobs, args.build_workers,
                       args.fuel, args.timeout)


# The game runner: this machine by default; modal_app.py splits it across containers.
PLAY = play_here


def play(games, work_dir, jobs, fuel, timeout):
    """games: list of (key, bot, opponent, map, seat). Returns {key: result}."""
    os.makedirs(work_dir, exist_ok=True)
    rows, where = [], {}
    for key, bot, opponent, map_path, seat in games:
        stem = os.path.join(work_dir, f"g{len(rows):05d}")
        a, b = (bot, opponent) if seat == "A" else (opponent, bot)
        rows.append(f"{a} {b} {map_path} {stem}.repl26a --statistics {stem}.json --json")
        where[key] = (stem, "teamA" if seat == "A" else "teamB")
    manifest = os.path.join(work_dir, "manifest.txt")
    open(manifest, "w", encoding="utf-8").write("\n".join(rows) + "\n")
    cmd = ["cambc", "run-batch", manifest, "--jobs", str(jobs), "--fuel-quantum", str(fuel), "--release",
           "--timeout", str(timeout), "--log-limit", "0"]
    with open(os.path.join(work_dir, "run.txt"), "w", encoding="utf-8") as log:
        subprocess.run(cmd, cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
    results = {}
    for key, (stem, team) in where.items():
        results[key] = read_result(stem + ".json", team)
        for ext in (".json", ".repl26a"):
            if os.path.exists(stem + ext):
                os.remove(stem + ext)
    return results


# ------------------------------------------------------------------ evolution
def mutate(rng, specs, values, scale):
    out = dict(values)
    for spec in specs:
        if rng.random() < 0.3:
            span = spec["hi"] - spec["lo"]
            out[spec["name"]] = clamp(spec, out[spec["name"]] + rng.gauss(0, scale * span))
    return out


def crossover(rng, specs, a, b):
    child = {}
    for spec in specs:
        x, y = a[spec["name"]], b[spec["name"]]
        r = rng.random()
        if r < 0.4:
            child[spec["name"]] = x
        elif r < 0.8:
            child[spec["name"]] = y
        else:  # a blend between them
            t = rng.random()
            child[spec["name"]] = clamp(spec, x + t * (y - x))
    return child


def tournament(rng, scored, k=3):
    return max(rng.sample(scored, min(k, len(scored))), key=lambda s: s[1])[0]


def sample_maps(rng, maps, count):
    return rng.sample(maps, min(count, len(maps)))


def evaluate_population(args, specs, population, gen, rng):
    """Plays the population and the control on one map sample; returns scores."""
    maps = sample_maps(rng, args.map_list, args.maps_per_gen)
    t0 = time.time()
    games = []
    for i in range(len(population)):
        for opponent in args.opponents:
            for m in maps:
                for seat in ("A", "B"):
                    games.append(((i, opponent, m, seat), i, opponent, m, seat))
    results = PLAY(args, specs, population, games, f"g{gen:03d}")
    scores = []
    for i in range(len(population)):
        rs = [r for k, r in results.items() if k[0] == i and r is not None]
        missing = sum(1 for k, r in results.items() if k[0] == i and r is None)
        fitness = sum(r[0] for r in rs) / len(rs) if rs else 0.0
        # Per map, so maps on which every individual scores the same can be dropped later.
        per_map = {}
        for k, r in results.items():
            if k[0] == i and r is not None:
                per_map.setdefault(os.path.basename(k[2]), []).append(r[0])
        scores.append({"fitness": fitness, "wins": sum(r[1] for r in rs), "games": len(rs),
                       "gem_share": sum(r[2] for r in rs) / len(rs) if rs else 0.0,
                       "faults": sum(r[3] for r in rs), "missing": missing,
                       "per_map": {m: round(sum(v) / len(v), 4) for m, v in sorted(per_map.items())}})
    print(f"gen {gen}: {len(games)} games on {len(maps)} maps in {time.time() - t0:.0f}s", flush=True)
    return scores, maps


def run(args):
    specs = load_params(args.params)
    defaults = {s["name"]: s["default"] for s in specs}
    state_path = os.path.join(args.out, "state.json")
    if args.resume and os.path.exists(state_path):
        state = json.load(open(state_path, encoding="utf-8"))
        rng = random.Random()
        rng.setstate((state["rng"][0], tuple(state["rng"][1]), state["rng"][2]))
        population, gen = state["population"], state["gen"]
    else:
        os.makedirs(args.out, exist_ok=True)
        rng = random.Random(args.seed)
        population = [defaults] + [mutate(rng, specs, defaults, 0.3) for _ in range(args.population - 1)]
        gen = 0
    history_path = os.path.join(args.out, "history.jsonl")
    while gen < args.generations:
        # Slot 0 is always the control (the current values), so every
        # generation measures how far the others are from it on the same games.
        population[0] = defaults
        scores, maps = evaluate_population(args, specs, population, gen, rng)
        control = scores[0]["fitness"]
        ranked = sorted(range(len(population)), key=lambda i: -scores[i]["fitness"])
        with open(history_path, "a", encoding="utf-8") as h:
            for i in range(len(population)):
                h.write(json.dumps({"gen": gen, "index": i, "control": i == 0, "values": population[i],
                                    **scores[i], "vs_control": scores[i]["fitness"] - control,
                                    "maps": maps}) + "\n")
        best = ranked[0]
        print(f"  best {scores[best]['fitness']:.4f} (control {control:.4f}, {scores[best]['wins']:.1f}/"
              f"{scores[best]['games']} wins)", flush=True)
        json.dump({"gen": gen, "values": population[best], **scores[best], "control": control},
                  open(os.path.join(args.out, "best.json"), "w", encoding="utf-8"), indent=1)
        # The next generation: elites, then children of tournament winners.
        scale = 0.3 - 0.2 * min(1.0, gen / max(1, args.generations - 1))
        scored = [(population[i], scores[i]["fitness"]) for i in range(len(population)) if i != 0]
        elites = [population[i] for i in ranked if i != 0][: args.elites]
        children = []
        while len(elites) + len(children) < args.population - 1:
            child = crossover(rng, specs, tournament(rng, scored), tournament(rng, scored))
            children.append(mutate(rng, specs, child, scale))
        population = [defaults] + elites + children
        gen += 1
        rng_state = rng.getstate()
        json.dump({"gen": gen, "population": population, "rng": [rng_state[0], list(rng_state[1]), rng_state[2]]},
                  open(state_path, "w", encoding="utf-8"))
    finalists(args)


def finalists(args):
    """Candidates by mean advantage over the control across every generation
    they were played in (a single generation's map sample is noisy)."""
    rows = [json.loads(l) for l in open(os.path.join(args.out, "history.jsonl"), encoding="utf-8")]
    # A generation whose games all went missing (a stopped run) measured nothing.
    rows = [r for r in rows if r["games"] > 0]
    seen = {}
    for r in rows:
        if r["control"]:
            continue
        key = json.dumps(r["values"], sort_keys=True)
        seen.setdefault(key, []).append(r["vs_control"])
    ranked = sorted(seen.items(), key=lambda kv: -(sum(kv[1]) / len(kv[1])) * min(1.0, len(kv[1]) / 3))
    out = [{"values": json.loads(k), "mean_vs_control": sum(v) / len(v), "generations": len(v)}
           for k, v in ranked[:5]]
    json.dump(out, open(os.path.join(args.out, "finalists.json"), "w", encoding="utf-8"), indent=1)
    print("finalists:", json.dumps(out[:3], indent=1))


def validate(args):
    """Plays a values file (best.json or one finalist) and the control on every
    map of --maps, both seats, against each opponent."""
    specs = load_params(args.params)
    defaults = {s["name"]: s["default"] for s in specs}
    chosen = json.load(open(args.validate, encoding="utf-8"))
    values = chosen[0]["values"] if isinstance(chosen, list) else chosen["values"]
    games = []
    for i in range(2):  # 0 the candidate, 1 the control
        for opponent in args.opponents:
            for m in args.map_list:
                for seat in ("A", "B"):
                    games.append(((i, opponent, m, seat), i, opponent, m, seat))
    results = PLAY(args, specs, [values, defaults], games, "validate")
    report = {}
    for i, name in enumerate(("candidate", "control")):
        rs = [r for k, r in results.items() if k[0] == i and r is not None]
        report[name] = {"fitness": sum(r[0] for r in rs) / len(rs), "wins": sum(r[1] for r in rs),
                        "games": len(rs), "faults": sum(r[3] for r in rs)}
    # Paired: games the candidate won and the control lost, and the reverse.
    better = worse = 0
    for k, r in results.items():
        if k[0] != 0 or r is None:
            continue
        c = results.get((1,) + k[1:])
        if c is None:
            continue
        better += r[1] > c[1]
        worse += r[1] < c[1]
    report["paired"] = {"candidate_only": better, "control_only": worse}
    json.dump(report, open(os.path.join(args.out, "validate.json"), "w", encoding="utf-8"), indent=1)
    print(json.dumps(report, indent=1))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bot", required=True, help="bot folder name under bots/ (baseline or test-bot)")
    p.add_argument("--params", required=True, help="gene specs (JSON list of name/type/lo/hi/default)")
    p.add_argument("--maps", required=True, help="text file: one map path per line, relative to the repo")
    p.add_argument("--opponents", required=True, help="comma-separated bot folders, relative to the repo")
    p.add_argument("--out", required=True)
    p.add_argument("--population", type=int, default=24, help="including the control")
    p.add_argument("--generations", type=int, default=15)
    p.add_argument("--elites", type=int, default=3)
    p.add_argument("--maps-per-gen", type=int, default=16)
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    p.add_argument("--build-workers", type=int, default=16)
    p.add_argument("--fuel", type=int, default=4_000_000, help="fuel quantum (evaluations use 4M)")
    p.add_argument("--timeout", type=int, default=1800, help="wall-clock seconds per game")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--resume", action="store_true")
    p.add_argument("--validate", help="values file to validate instead of evolving")
    args = p.parse_args()
    args.out = os.path.abspath(args.out)
    args.bot_src = os.path.join(REPO, "bots", args.bot, "src")
    args.opponents = [os.path.abspath(os.path.join(REPO, o)) for o in args.opponents.split(",") if o]
    args.map_list = [os.path.abspath(os.path.join(REPO, l.strip())) for l in open(args.maps, encoding="utf-8")
                     if l.strip() and not l.startswith("#")]
    for path in [args.bot_src] + args.opponents + args.map_list:
        if not os.path.exists(path):
            sys.exit(f"missing: {path}")
    if args.validate:
        validate(args)
    else:
        run(args)


if __name__ == "__main__":
    main()
