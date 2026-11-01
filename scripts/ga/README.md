# Genetic algorithm over `policy::` constants

`ga.py` tunes one bot's `strategy.h` constants by playing games through
`cambc run-batch`; `modal_app.py` runs it on Modal, split across three 64-core containers.

## Which bot, on which maps

The plan was a hybrid: a map classifier choosing baseline or test-bot per map,
each tuned on its own maps. The labels said otherwise. Baseline against
test-bot on 202 maps (the 70 eval maps and the 132 generated and edge maps in
`maps/ga`), both seats, 404 games at 4M fuel:

| Baseline wins of the two seats | Maps |
| --- | --- |
| 2 | 86 |
| 1 | 83 |
| 0 | 33 |

Baseline won 255 of 404. No map feature separates the 33: under 5-fold
cross-validation the best single threshold agrees with 60.9% of the labels,
below always picking baseline (63.1%). But test-bot comes closest on dense
maps (43% against baseline on maps with 60+ camps per 1000 walkable tiles,
35% on sparse ones), so each bot is tuned for one side:

- baseline on `maps_sparse.txt` (96 maps, under 40 camps per 1000 walkable tiles)
- test-bot on `maps_dense.txt` (100 maps, 24 or more)

The 29 maps between 24 and 40 are in both, so neither bot collapses near the
cut. Each winner is validated on its half of the holdout (`holdout_sparse.txt`,
`holdout_dense.txt`, split at 32). A hybrid that picks by density follows only
if tuned test-bot then beats tuned baseline on dense maps.

## Files

| File | Contents |
| --- | --- |
| `ga.py` | The GA (standard library only) |
| `params_baseline.json`, `params_test-bot.json` | Genes: name, type, range, current value (the control) |
| `maps_sparse.txt`, `maps_dense.txt` | Training maps for baseline and test-bot, from `maps/ga` (35 real, 100 generated, 32 edge) |
| `holdout.txt`, `holdout_sparse.txt`, `holdout_dense.txt` | The 35 eval maps never trained on, and their sparse and dense halves |
| `opponents/opp_baseline`, `opponents/opp_testbot` | Frozen copies of our two bots (named apart from `bots/`: run-batch keys builds by folder name) |
| `opponents/opp_rusher`, `opp_farmer`, `opp_turtle`, `opp_thief`, `opp_deathball` | Extreme styles built from baseline by `make_opponents.py`: always attacks; never fights; farms only its own half; prefers the enemy's camps; moves as one |
| `make_opponents.py` | Rebuilds the five extreme opponents from `bots/baseline/src` |
| `modal_app.py` | Modal launcher: a coordinator plus three 64-core game workers |
| `results/baseline_s1`, `results/test-bot_s1` | The first runs (2026-10-31): `history.jsonl` (every individual, every generation, with per-map scores), `finalists.json` (top 5; #1 went into `bots/`), `validate.json` (holdout result), `spent.json` (estimated Modal spend) |
| `mapgen/` | Map generators for `maps/ga`: `genreal.mjs` (the 100 realistic maps, from `realstats_ga.json`, the GA-half real maps' statistics), `genmaps.mjs` (the edge maps), `realstats.mjs` (map statistics), `maplib.mjs` (map building over the engine module). They need the visualiser's engine module (`engine-v2_0_0.wasm`, cambc 4.0.0), not committed |

## Results of the first runs (2026-10-31)

| | baseline (sparse maps) | test-bot (dense maps) |
| --- | --- | --- |
| Generations | 24 (stopped at the Modal limit) | 29 |
| Elites vs control, late in the run | about +0.03, flat from generation 15 | about +0.045, flat from generation 15 |
| Holdout, paired games (finalist won only : control won only) | 43 : 32 (18 sparse maps, within noise) | 53 : 25 (17 dense maps, significant) |
| All 70 eval maps x 2 seats at 4M, wins vs the frozen test-bot copy | 86/140 (control 83) | 77/140 (control 67) |

Both finalists are in `bots/` since 5a330e7, and the gene defaults in the
`params_*.json` files are now those values, so the next run's control is the
current bot. Genes that measurably move fitness (within-generation
correlation, |z| > 2): baseline 8 of 42 (`late_gold_value`,
`step_incoming_weight`, `health_pull_damage`, `cohesion_hazard`,
`cohesion_max_spacing`, `travel_hazard`, `worker_sighting_memory`,
`pursue_health`), test-bot 16 of 58 (led by `partner_penalty`,
`retreat_health`, `worker_sighting_penalty`, `prism_safe_gem_factor`,
`respawn_wait_weight`, `stuck_rounds`, `prism_bag`, `aggression`).
`aggression` and `lock_memory` ended at the top of their ranges.

## How it works

- Each individual is a copy of the bot with its constants written into
  `strategy.h`. Slot 0 is always the current values, the control.
- A generation plays every individual on the same freshly sampled maps, both
  seats, against each opponent. The Modal defaults are 40 individuals over 25 generations, 6 maps
  and 5 opponents (our two bots, turtle, thief and deathball; rusher and farmer
  give little signal): 2,400 games a generation, 60,000 a run, about $14.
- Fitness per game: 0.5 × win (draw 0.5) + 0.5 × our share of the submitted
  gems, read from the game's statistics file. A bot fault counts as a loss.
- Selection: 3 elites, then children of 3-way tournament winners. Each gene
  comes from either parent or a blend; mutation is Gaussian, 30% of a gene's
  range falling to 10% over the run.
- Each generation appends to `history.jsonl`, writes `best.json`, and
  checkpoints `state.json` for `--resume`.
- At the end, `finalists.json` ranks values by their mean advantage over the
  control across every generation they played. A single generation's sample
  is noisy.
- `--validate <values file> --maps scripts/ga/holdout.txt` plays a candidate
  and the control on every listed map. Do that on the holdout, then on all
  140 eval games, before adopting anything.

## Running

Locally, from the repository root (keep `--out` short on Windows: deep build
paths pass its 260-character limit):

```sh
python scripts/ga/ga.py --bot baseline --params scripts/ga/params_baseline.json \
    --maps scripts/ga/maps_sparse.txt \
    --opponents scripts/ga/opponents/opp_testbot,scripts/ga/opponents/opp_baseline \
    --out ga_runs/baseline --jobs 14
```

On Modal (`pip install modal`, `modal setup` once):

```sh
modal run --detach scripts/ga/modal_app.py --bot baseline   # sparse maps
modal run --detach scripts/ga/modal_app.py --bot test-bot   # dense maps
modal run scripts/ga/modal_app.py --bot baseline --resume
modal run scripts/ga/modal_app.py --bot baseline --validate finalists.json --maps scripts/ga/holdout_sparse.txt
modal run scripts/ga/modal_app.py --bot test-bot --validate finalists.json --maps scripts/ga/holdout_dense.txt
modal volume get battlecode-ga baseline_s1 ./ga_runs/
```

A small coordinator container (2 cores) runs the GA; each generation's games
are split across `WORKERS` (3) containers of `CPUS` (64, Modal's per-container
limit) cores. A Modal core is a physical core with two threads, so each worker
plays 120 games at a time (`JOBS`) and reserves 32 GB (`MEMORY`; at 16 GB Modal killed a worker for exceeding its
request). Games come grouped by individual, so
each worker builds only its share of the individuals. `--detach` keeps a run
going after the local terminal closes.

`--max-dollars` (default 15) caps a run's estimated spend: the workers' wall
time times Modal's standard rates (it matched Modal's billing), kept in `spent.json` in the run folder.
Before each batch of games the coordinator stops if the last batch's cost would
take the total past the cap; `--resume` with a higher cap continues. Each
history row also has `per_map`, the individual's mean fitness per map, for
finding maps on which every individual scores the same.
The image installs cambc 4.0.0's Linux wheel and builds both bots once, which
downloads cambc's toolchain.
