"""Runs scripts/ga/ga.py on Modal: a small coordinator container runs the GA,
and each generation's games are split across WORKERS containers of CPUS cores
(Modal's per-container limit is 64), each building only the individuals its
share of the games uses.

The image has cambc 4.0.0 (its Linux wheel) and the repository; a first
build there downloads cambc's toolchain, done once while the image is built.
Results go to the Modal volume `battlecode-ga`, so a run can be resumed or
read afterwards (`modal volume get battlecode-ga <run>/best.json .`).

  modal run scripts/ga/modal_app.py --bot baseline
  modal run scripts/ga/modal_app.py --bot test-bot --generations 20 --resume
  modal run scripts/ga/modal_app.py --bot baseline --validate finalists.json --maps scripts/ga/holdout_sparse.txt
  modal run scripts/ga/modal_app.py --bot test-bot --validate finalists.json --maps scripts/ga/holdout_dense.txt
"""
import json
import os
import sys
import time

import modal

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CPUS = 64  # Modal's per-container limit; a Modal core is a physical core, two threads
WORKERS = 3
JOBS = 120  # games at a time per worker, one per thread
MEMORY = 32 * 1024  # MiB reserved per worker; at 16 GiB Modal killed a worker playing 120 games
# Modal's standard rates: per physical core and per GiB of memory, per second.
CORE_RATE, GIB_RATE = 0.0000131, 0.00000222
WORKER_RATE = CPUS * CORE_RATE + MEMORY / 1024 * GIB_RATE

image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install("cmake", "ninja-build", "git")
    .pip_install("cambc==4.0.0")
    .add_local_dir(REPO, "/repo", copy=True,
                   ignore=["**/.cambc/**", "**/compile_commands.json", "analysis/**", "ga_runs/**", ".git/**"])
    # The toolchain download and a first build, once, in the image.
    .run_commands("cd /repo && cambc build baseline --release && cambc build test-bot --release")
)
volume = modal.Volume.from_name("battlecode-ga", create_if_missing=True)
app = modal.App("battlecode-ga", image=image)


def load_ga():
    os.chdir("/repo")
    sys.path.insert(0, "/repo/scripts/ga")
    import ga
    return ga


@app.function(cpu=CPUS, memory=MEMORY, timeout=12 * 3600)
def play_shard(bot_src, specs, values_list, games, tag, fuel, timeout):
    # Builds and games stay on the worker's own disk; only results go back.
    return load_ga().play_values(bot_src, specs, values_list, games, "/tmp/work", tag, JOBS, 16, fuel,
                                 timeout)


class BudgetReached(Exception):
    pass


def play_split(args, specs, values_list, games, tag):
    # The spend so far: the workers' wall time (start-up and idle included)
    # times their rate, kept in the run folder so --resume continues it. It
    # matched Modal's billing on the first runs.
    spent_path = os.path.join(args.out, "spent.json")
    spent = json.load(open(spent_path)) if os.path.exists(spent_path) else {"dollars": 0.0, "last": 0.0}
    if MAX_DOLLARS and spent["dollars"] + spent["last"] > MAX_DOLLARS:
        raise BudgetReached(f"${spent['dollars']:.2f} spent; the next batch (~${spent['last']:.2f}) would pass "
                            f"the ${MAX_DOLLARS:.2f} cap")
    # Games come grouped by individual, so contiguous shares keep each
    # individual's games (and its build) on one worker.
    share = -(-len(games) // WORKERS)
    parts = [games[i:i + share] for i in range(0, len(games), share)]
    t0 = time.time()
    results = {}
    for part in play_shard.starmap([(args.bot_src, specs, values_list, part, tag, args.fuel, args.timeout)
                                    for part in parts]):
        results.update(part)
    cost = (time.time() - t0) * len(parts) * WORKER_RATE
    spent = {"dollars": spent["dollars"] + cost, "last": cost}
    json.dump(spent, open(spent_path, "w"))
    print(f"  batch ~${cost:.2f}, run total ~${spent['dollars']:.2f}", flush=True)
    return results


MAX_DOLLARS = 0.0


@app.function(cpu=2, memory=4096, timeout=24 * 3600, volumes={"/runs": volume})
def evolve(argv: list[str], max_dollars: float):
    global MAX_DOLLARS
    MAX_DOLLARS = max_dollars
    ga = load_ga()
    ga.PLAY = play_split
    sys.argv = ["ga.py", *argv]
    try:
        ga.main()
    except BudgetReached as stop:
        print(f"stopped: {stop}. Continue with --resume and a higher --max-dollars.", flush=True)
    finally:
        volume.commit()


# Our two bots and three extreme styles (make_opponents.py). The rusher and the
# farmer are left out: baseline beats them 138 and 104 of 140, little signal.
OPPONENTS = ",".join(f"scripts/ga/opponents/opp_{name}" for name in
                     ("baseline", "testbot", "turtle", "thief", "deathball"))


@app.local_entrypoint()
def main(bot: str = "baseline", generations: int = 25, population: int = 40, maps_per_gen: int = 6,
         seed: int = 1, resume: bool = False, validate: str = "", maps: str = "", opponents: str = "",
         run: str = "", max_dollars: float = 15.0):
    run = run or f"{bot}_s{seed}"
    argv = ["--bot", bot, "--params", f"scripts/ga/params_{bot}.json",
            # Baseline is tuned for sparse maps, test-bot for dense ones.
            "--maps", maps or ("scripts/ga/maps_dense.txt" if bot == "test-bot" else "scripts/ga/maps_sparse.txt"),
            "--opponents", opponents or OPPONENTS,
            "--out", f"/runs/{run}",
            "--generations", str(generations), "--population", str(population),
            "--maps-per-gen", str(maps_per_gen), "--seed", str(seed)]
    if resume:
        argv.append("--resume")
    if validate:
        argv += ["--validate", f"/runs/{run}/{validate}"]
    evolve.remote(argv, max_dollars)
