"""One evaluation on Modal: 140 games, the 70 maps of maps/eval from both seats against
the live bots/test-bot, one core per game and every game at once. Four containers of 35
cores (a container holds at most 64) each build the bot and play their 35 games.

Each run writes <out>/<name>/run.txt with the engine's result lines, the format
run-batch prints locally, and prints the wins. Runs in one call play one after the
other, each with its own 140 cores.

  modal run scripts/modal_eval.py --runs "mine=C:/path/to/botfolder" --out C:/some/dir
  modal run scripts/modal_eval.py --runs "a=C:/x/a,b=C:/x/b" --out C:/some/dir --replays

A bot folder holds src/. --replays also brings the 140 replays back (65 KB each or so).
--max-dollars stops before a run that would pass the cap, counted from wall time x rate.
"""
import io
import os
import re
import subprocess
import tarfile
import tempfile
import time

import modal

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
WORKERS = 4
CORES = 35  # One game a core: a Modal core is a physical core, two threads.
MEMORY = 16 * 1024  # MiB per container; the GA's 120 games needed more than 16 GiB, 32 was enough.
# Modal's rates per physical core and per GiB of memory, per second.
CORE_RATE, GIB_RATE = 0.0000131, 0.00000222
RATE = WORKERS * (CORES * CORE_RATE + MEMORY / 1024 * GIB_RATE)
SKIP = ["**/.cambc/**", "**/compile_commands.json"]

# Only what a game needs, so editing the bot or the docs does not rebuild the image.
# The toolchain download and the opponent's build happen once, here.
image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install("cmake", "ninja-build", "git")
    .pip_install("cambc==4.0.0")
    .add_local_file(os.path.join(REPO, "cambc.toml"), "/repo/cambc.toml", copy=True)
    .add_local_dir(os.path.join(REPO, "cmake"), "/repo/cmake", copy=True)
    .add_local_dir(os.path.join(REPO, "sdk"), "/repo/sdk", copy=True)
    .add_local_dir(os.path.join(REPO, "maps", "eval"), "/repo/maps/eval", copy=True)
    .add_local_dir(os.path.join(REPO, "bots", "test-bot"), "/repo/bots/test-bot", copy=True, ignore=SKIP)
    .run_commands("cd /repo && cambc build test-bot --release")
)
app = modal.App("battlecode-eval", image=image)


@app.function(cpu=CORES, memory=MEMORY, timeout=3600)
def play_shard(run, source, rows, fuel, replays):
    """rows: (seat, map name). Returns the batch log and, if asked, the replays."""
    start = time.time()
    work = tempfile.mkdtemp(prefix="work")  # A container can take a second shard after its first.
    folder = f"{work}/{run}"
    os.makedirs(folder)
    with tarfile.open(fileobj=io.BytesIO(source)) as tar:
        tar.extractall(folder, filter="data")
    built = subprocess.run(["cambc", "build", folder, "--release"], cwd="/repo", capture_output=True, text=True,
                           errors="replace")
    if built.returncode != 0:
        raise RuntimeError("build failed:\n" + "\n".join((built.stdout + built.stderr).strip().splitlines()[-20:]))
    print(f"{run}: built in {time.time() - start:.0f}s", flush=True)
    out = f"{work}/out/{run}"
    os.makedirs(out)
    lines = []
    for seat, name in rows:
        a, b = (folder, "/repo/bots/test-bot") if seat == "A" else ("/repo/bots/test-bot", folder)
        lines.append(f"{a} {b} /repo/maps/eval/{name}.map26a {out}/{seat}-{name}.repl26a")
    manifest = f"{work}/manifest.txt"
    open(manifest, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    done = subprocess.run(["cambc", "run-batch", manifest, "--jobs", str(len(rows)), "--fuel-quantum", str(fuel),
                           "--release"], cwd="/repo", capture_output=True, text=True, errors="replace")
    if done.returncode != 0:
        raise RuntimeError("run-batch failed: " + (done.stdout + done.stderr).strip()[-2000:])
    print(f"{run}: {len(rows)} games done at {time.time() - start:.0f}s", flush=True)
    saved = {}
    if replays:
        for seat, name in rows:
            path = f"{out}/{seat}-{name}.repl26a"
            if os.path.exists(path):
                saved[f"{seat}-{name}.repl26a"] = open(path, "rb").read()
    return done.stdout, saved


def pack(bot):
    """The bot folder's src/ as tar bytes, without build output."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
        tar.add(os.path.join(bot, "src"), arcname="src",
                filter=lambda t: None if ".cambc" in t.name or t.name.endswith("compile_commands.json") else t)
    return buffer.getvalue()


@app.local_entrypoint()
def main(runs: str, out: str, fuel: int = 1000000, replays: bool = False, max_dollars: float = 5.0):
    names = sorted(f[:-len(".map26a")] for f in os.listdir(os.path.join(REPO, "maps", "eval")) if f.endswith(".map26a"))
    rows = [(seat, name) for name in names for seat in "AB"]
    share = -(-len(rows) // WORKERS)
    parts = [rows[i:i + share] for i in range(0, len(rows), share)]
    spent = last = 0.0
    for spec in runs.split(","):
        run, bot = spec.split("=", 1)
        target = os.path.join(out, run)
        if os.path.exists(target):
            raise SystemExit(f"{target} exists")
        if spent + last > max_dollars:
            raise SystemExit(f"stopped before {run}: ${spent:.2f} spent, the ${max_dollars:.2f} cap would pass")
        source = pack(bot)
        t0 = time.time()
        results = list(play_shard.starmap([(run, source, part, fuel, replays) for part in parts]))
        last = (time.time() - t0) * RATE
        spent += last
        os.makedirs(target)
        wins = games = 0
        with open(os.path.join(target, "run.txt"), "w", encoding="utf-8") as log:
            for text, saved in results:
                log.write(text)
                for file, data in saved.items():
                    open(os.path.join(target, file), "wb").write(data)
                winner = None
                for line in text.splitlines():
                    m = re.search(r"TEAM ([AB]) WIN", line)
                    if m:
                        winner = m.group(1)
                    r = re.search(r"Replay: .*/([AB])-[^/]*\.repl26a", line)
                    if r and winner:
                        games += 1
                        wins += winner == r.group(1)
                        winner = None
        print(f"{run}: {wins}/{games} in {time.time() - t0:.0f}s, about ${last:.2f}; total about ${spent:.2f}", flush=True)
