# Genetic algorithm over `policy::` constants

`ga.py` tunes one bot's `strategy.h` constants by playing games through
`cambc run-batch`; `modal_app.py` runs it on one large Modal container.

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
| `modal_app.py` | Modal launcher |

## How it works

- Each individual is a copy of the bot with its constants written into
  `strategy.h`. Slot 0 is always the current values, the control.
- A generation plays every individual on the same freshly sampled maps, both
  seats, against each opponent. The Modal defaults are 40 individuals over 25 generations, 10 maps
  and 7 opponents (our two bots and the five extreme styles): 5,600 games a generation, 140,000 a run.
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
modal run scripts/ga/modal_app.py --bot baseline          # sparse maps
modal run scripts/ga/modal_app.py --bot test-bot          # dense maps
modal run scripts/ga/modal_app.py --bot baseline --resume
modal run scripts/ga/modal_app.py --bot baseline --validate finalists.json --maps scripts/ga/holdout_sparse.txt
modal run scripts/ga/modal_app.py --bot test-bot --validate finalists.json --maps scripts/ga/holdout_dense.txt
modal volume get battlecode-ga baseline_s1 ./ga_runs/
```

`CPUS` in `modal_app.py` sets the container size (192) and `--jobs` (192 − 2).
The image installs cambc 4.0.0's Linux wheel and builds both bots once, which
downloads cambc's toolchain.
