# Imitating print dont int

Goal: learn, per hero, decision trees that reproduce what print dont int's bots do each turn, using
only what that bot could see at the time. Then read the trees and the macro report to understand
their strategy, and export the trees to C++ so a bot can play the same way.

## Pipeline

All commands are run from `analysis/print_dont_int/`. They are resumable: rerunning skips finished
work.

| step | command | output | time |
|---|---|---|---|
| 0. install | `pip install -r training/requirements.txt` (and node >= 18) | | |
| 1. extract turns | `python training/extract/run_extract.py` | `training/data/raw/<match>.jsonl.gz`, `training/data/games.csv` | ~3 min |
| 2. features | `python training/build_dataset.py` | `training/data/features/<match>.npz`, `feature_spec.json` | ~8 min |
| 3. macro report | `python training/macro_report.py` | `training/reports/macro/report.md` + plots | ~2 min |
| 4. train | `python training/train.py --hero all --head all` | `checkpoints/<hero>/<head>/{current,best}/` + `checkpoints/summary.csv` | see below |
| 5. export | `python training/export_cpp.py --verify-cpp` | `training/cpp/generated/*.h` | seconds |

Steps 1–3 have been run once already. Step 4 (training) has not been run.

Train one model at a time while iterating: `python training/train.py --hero vesper --head move`.
`--resume` continues from `current/`. Useful flags:

- `--rounds` (default 600) and `--patience` (default 60): early stopping uses validation log-loss.
- `--ckpt-every` (default 25): how often to checkpoint and plot.
- `--max-depth` (default 6) and `--lr`.
- `--max-rows` (default 600k): caps the rows used per head.
- `--balance sqrt`: up-weights rare classes. The default reproduces the true frequencies.

The first run per hero caches the hero's rows under `data/cache/`.

## How the data is made (`extract/`)

The replays (`../replays/*.zip`) are re-simulated in node, using the cambc visualiser's own engine
module (`v2_0_0-*.wasm`, found automatically or set with `CAMBC_ENGINE_WASM`).

- **Observations:** at the moment each print dont int champion's turn waits for input, the
  extractor queries the engine through the same `Game_API_*` host functions that the bot SDK
  calls, from that champion's perspective. Vision is enforced by the engine itself, so an
  observation contains exactly what that bot could have read: its own state, visible enemies,
  allies and camps, the camp groups in vision, altar locks, teleports and submitted gems.
- **Actions:** every action of the turn is recorded from the timeline steps.

Step kinds decoded from replays: 4 move, 5 attack, 6 ability, 8 end turn, 9 set priority,
11 recall, 12 submit gems, 13 buy, 14 ward, 15 consumable, 16 cancel channel, 18 tribute done,
20 altar lock, 21/22 teleport, 23 blocked move, 25 sell, 26 fuel exhausted.

Six replays desync in the engine at round 1–2 and are skipped. That leaves 494 games and about
1.75M champion turns.

## Features (`features.py`, 285 floats per turn; NaN = not observable)

- **Context:** round, submitted gems for both teams, map size, altars, camp groups, team sizes,
  alive counts.
- **Self:** health, strength, focus, gold, gems, cooldowns, ward, channel and state flags, stun,
  shield. Also bush and spawn flags, and path distance to own spawn, enemy spawn and the nearest
  altar.
- **Items:** shard and emblem counts by stat, emblem families, item value, consumables.
- **Enemies:** 3 slots, plus aggregates such as health within 4 tiles and the most gems carried.
  Memory adds rounds since an enemy was last seen.
- **Allies:** 2 slots, plus health-ratio aggregates.
- **Visible camps:** 4 slots. Each has type, health, awake flag, the group's gem value, and how
  many of the group are left. Also counts and strength of the camps currently able to hit you.
- **Camp groups, with memory:** each champion remembers what it last saw of every group and
  when respawns are due. From that: the nearest live group and the best-value one (exact path
  distance, rewards, composition), the nearest uncaptured reward, and the capture state of the
  tile it is standing on.
- **Per move direction** (8): legal, the change in path distance to spawn, altar, nearest group
  and best group, the change in distance to the nearest enemy and camp, camps in range after the
  step, enemies within 3, and bush.
- **Teleport opportunity:** the best path-distance saving from any landing tile within 3, towards
  spawn, altar, the best group and the enemy spawn. Teleports are used about once per hero per
  game, so these "interesting regions" are computed rather than learned from map tiles.

We ignore the 8-byte shared memory and the 1024-byte draft memory, since we can't see what they
put there. Memory here is each champion's own, because vision is per champion.

`check_parity.py` confirms that the Python path distances equal the SDK's `distance_field` tile
for tile on the most heavily walled maps. That is what lets the C++ bot compute the same features
cheaply.

## Heads (one XGBoost model per hero × head)

Rows are champion turns; each head only uses the turns where it applies.

| head | classes | rows |
|---|---|---|
| `cancel` | yes/no | turns that start in a recall or tribute channel |
| `move` | 8 directions + stay | turns free to act (alive, not stunned, not channeling) |
| `attack` | none, enemy slot 0–2, camp slot 0–3, other enemy, other camp, altar lock | free turns |
| `ability` | none / slot 0 / slot 1 | free turns |
| `ability_target` | target slot, as for `attack` | unit- or ground-targeted casts |
| `dash` | landing offset (5×5) | dash casts |
| `ward`, `ward_offset` | place? and where (5×5) | free turns |
| `submit` | tribute now? | carrying gems, next to an altar |
| `teleport`, `tp_offset` | use crystal? and where (7×7) | holding a crystal |
| `lock` | place altar lock? | holding a lock |
| `recall` | start a recall this turn? | free turns |
| `recall_commit` | does that recall really bring them home? | analysis only |
| `shop` | next shard / consumable, or stop | each shop decision on spawn or while dead |

The split is by game: 70% train, 10% validation (early stopping, best checkpoint), and 20% test,
which is used once at the end. Each checkpoint folder holds:

- `model.json` and `state.json` (history, class map, metrics);
- `curves.png` (train/val loss, accuracy and balanced accuracy, majority baseline);
- `confusion.png` and `importance.png`;
- in `best/` only: `test_metrics.json` and `test_confusion.png`.

## What we already know (training/reports/macro/report.md)

- **Record:** 77% wins over these 500 games. All losses are on gems, never by timeout.
- **Draft:** fixed order by team size. With 3: Vesper > Marcella > HoR. With 4: Vesper > HoR >
  Vendetta > Marcella. With 5: Vesper > HoR > Vendetta > Ryth > Marcella.
- **Speculative recall:** they start a recall at the end of about 83% of turns and cancel it at the
  start of the next turn when they want to act. Committing to go home is then just *not
  cancelling*, which saves a channel tick.
- **Time split:** about 45–58% of turns walking, 17–27% farming camps, 4–6% fighting champions,
  and 5–9% dead. Mostly wolves and elks, rarely golems or monoliths.
- **Builds:** they open with a Treasure shard and then buy an altar lock. The main lines are
  Bruiser str/hp for Vesper, Bruiser focus for Marcella, Treasure and Power focus for HoR, and
  Treasure str for Vendetta, with Durability hp throughout. They buy teleport crystals and altar
  locks often but use them rarely.

## C++ (`cpp/`)

- **`pdi_tree.h`:** the tree evaluator. Splits go left when `x < threshold`, and NaN takes the
  learned default branch.
- **`pdi_features.h`:** a line-for-line port of `features.py` using the SDK, including the
  champion's memory. Keep the two in sync and bump `FEATURE_VERSION`; a `static_assert` checks
  it against the exported models.
- **`pdi_policy.h`:** `pdi::Policy::play_turn()` and `pdi::draft()`. The turn flow is: dead →
  shop; channeling → cancel?; on spawn → shop; then ward, tribute, teleport, lock,
  attack/ability/move; and at the end of the turn, the speculative recall. Moves are masked to
  legal directions and every action is guarded.
- **`generated/`:** written by `export_cpp.py`. `export_cpp.py` first checks the exported arrays
  against XGBoost's own output margins, and `--verify-cpp` compiles them with g++ and compares
  again.

To make a bot, copy `bots/starter-cpp` (or `baseline`), export into it, and call the policy from
`main.cpp` (snippet at the top of `pdi_policy.h`):

```
python training/export_cpp.py --out ../../bots/pdi-clone/src/pdi/generated --verify-cpp
```

**Fuel:** each call walks about rounds × classes × depth tree levels, at roughly 12 wasm
instructions per level. Fully trained models (600 rounds) cost about 1M fuel per turn, so they
can't be shipped as they are.

Boosted trees are sequential, so the exporter keeps only the **first N rounds** of each head,
which is still a complete model. The default budget in `export_cpp.py` (`BUDGET`) keeps a typical
acting turn near 40k fuel. Override it per head with `--rounds move=60,attack=40`.

- `python training/budget_curve.py --hero vesper` shows the validation accuracy at each cut.
- The exporter prints the cost of every model and of a typical turn.

The policy also stops consulting heads once `fuel_reserve` is reached.

**Checking the port:** set `policy.dump_features = true`, play a local game, then run
`python training/compare_features.py --log run.log --replay <zip> --team <0|1>`.

## Files

```
extract/engine.mjs         engine wrapper (replay timeline + perspective-scoped Game_API queries)
extract/extract_game.mjs   one replay -> per-turn observations and actions
extract/run_extract.py     all replays, parallel + resumable
extract/sdk_distance.mjs   the SDK's distance_field on a map (parity check)
features.py                observation -> features, actions -> labels
build_dataset.py           raw -> features/*.npz
train.py                   XGBoost per hero x head, with checkpoints, plots and test eval
export_cpp.py              best models -> C++ headers (+ verification)
macro_report.py            strategy summary
check_parity.py            Python BFS vs SDK distance_field
compare_features.py        C++ feature dump vs features.py
cpp/                       pdi_tree.h, pdi_features.h, pdi_policy.h, generated/
```
