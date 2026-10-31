"""Runs scripts/ga/ga.py on one large Modal container.

The image has cambc 4.0.0 (its Linux wheel) and the repository; a first
build there downloads cambc's toolchain, done once while the image is built.
Results go to the Modal volume `battlecode-ga`, so a run can be resumed or
read afterwards (`modal volume get battlecode-ga <run>/best.json .`).

  modal run scripts/ga/modal_app.py --bot baseline
  modal run scripts/ga/modal_app.py --bot test-bot --generations 20 --resume
  modal run scripts/ga/modal_app.py --bot baseline --validate finalists.json --maps scripts/ga/holdout_sparse.txt
  modal run scripts/ga/modal_app.py --bot test-bot --validate finalists.json --maps scripts/ga/holdout_dense.txt
"""
import os
import subprocess

import modal

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CPUS = 192

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


@app.function(cpu=CPUS, memory=CPUS * 1024, timeout=24 * 3600, volumes={"/runs": volume})
def evolve(argv: list[str]):
    subprocess.run(["python", "scripts/ga/ga.py", *argv], cwd="/repo", check=True)
    volume.commit()


# Our two bots and the five extreme styles (make_opponents.py).
OPPONENTS = ",".join(f"scripts/ga/opponents/opp_{name}" for name in
                     ("baseline", "testbot", "rusher", "farmer", "turtle", "thief", "deathball"))


@app.local_entrypoint()
def main(bot: str = "baseline", generations: int = 25, population: int = 40, maps_per_gen: int = 10,
         seed: int = 1, resume: bool = False, validate: str = "", maps: str = "", opponents: str = "",
         run: str = ""):
    run = run or f"{bot}_s{seed}"
    argv = ["--bot", bot, "--params", f"scripts/ga/params_{bot}.json",
            # Baseline is tuned for sparse maps, test-bot for dense ones.
            "--maps", maps or ("scripts/ga/maps_dense.txt" if bot == "test-bot" else "scripts/ga/maps_sparse.txt"),
            "--opponents", opponents or OPPONENTS,
            "--out", f"/runs/{run}", "--jobs", str(CPUS - 2), "--build-workers", "32",
            "--generations", str(generations), "--population", str(population),
            "--maps-per-gen", str(maps_per_gen), "--seed", str(seed)]
    if resume:
        argv.append("--resume")
    if validate:
        argv += ["--validate", f"/runs/{run}/{validate}"]
    evolve.remote(argv)
