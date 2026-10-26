# Lucky Gold Clusters

Our Cambridge Battlecode 2026 bots. [`bots/baseline`](bots/baseline) is the main bot: a copy of the top ladder team willow best girl's play, updated for engine 2.0.0. [`bots/test-bot`](bots/test-bot) is our earlier bot (formerly `newest-bot`).

## Repository layout

| Path | Contents |
| --- | --- |
| `bots/baseline/` | The main C++ bot: willow best girl's play, with its engine 2.0.0 draft and builds (formerly `willow-copy`) |
| `bots/test-bot/` | Our earlier C++ bot, with its engine 1.x draft and builds (formerly `newest-bot`; see below) |
| `bots/starter-cpp/` | The SDK starter bot, used as a sparring opponent |
| `docs/GameRules.md`, `docs/BotAPICpp.md` | Exported game handbook and C++ API reference |
| `docs/design-notes.md` | Squad design spec, with implementation status and archived rosters |
| `docs/squad-controller.mmd` | Mermaid flowchart of the squad controller |
| `scripts/evaluate.py` | **The standard check** of whether a change beats `bots/baseline` (see below) |
| `scripts/ladder.ps1` | Quick per-map table, one game per map (too noisy to judge a change) |
| `scripts/benchmark.ps1` | Grid search over `policy::` constants in `strategy.h` |
| `analysis/` | Exported match analytics |
| `maps/` | Current map pool (`cambc maps pull` refreshes it) |
| `sdk/`, `cmake/` | Vendored SDK and the CMake project `cambc` builds with |

## Building and running

```sh
cambc build baseline
cambc run baseline starter-cpp --map contest
pwsh scripts/ladder.ps1 -Build                      # every map vs starter-cpp
pwsh scripts/ladder.ps1 -Opponents starter-cpp,old  # any folders under bots/
pwsh scripts/benchmark.ps1 -GridSpec "commit_bonus=1.0f,1.08f,1.2f"
```

Both scripts can be run from any directory.

### Judging a change

Matches are deterministic but chaotic: a one-line change can flip a third of single games, so a single ladder pass proves nothing. `scripts/evaluate.py` plays every map in both seats at five fuel budgets near the real 100k limit, against one or more opponents (`--baseline a,b,c`). It builds each bot once and runs the matches through `cambc run-batch` in release mode, as on the platform, using at most 90% of your CPU cores at below-normal priority so the machine stays usable.

```sh
python scripts/evaluate.py --mirror                     # current bot, plus a baseline-vs-baseline noise floor
python scripts/evaluate.py off:commit_bonus=1.0f current  # compare policy variants side by side
python scripts/evaluate.py --maps corners,swirl --fuels 100000
```

The mirror run scores 50% by construction. Differences of about ±7 wins in 170 games are noise. Only commit bot changes that beat the baseline by more than that. Replays, including each bot's stdout logs (inside the `.repl26a` zip), are kept under `--out`.

## How test-bot is organised

`src/` is grouped by controller. The CMake build compiles every `.cpp` file under `src/` recursively.

| Folder | Role |
| --- | --- |
| `main.cpp`, `bot.h`, `bot.cpp` | Entry point, the `Bot` class, per-turn dispatch |
| `strategy.h` | Tunable `policy::` constants, priority tiers, the shared-memory format and the draft-memory format |
| `draft/` | Fixed roster per pick count, and the opening plan written to draft memory |
| `squad/` | Stellar/Tavi/Cassi controller: coordination, job selection and estimates, travel, delivery |
| `workers/` | Independent policy for Issac, Ryth and Varg |
| `mechanics/` | Shared execution both controllers use: observation, movement, combat, combo micro, shopping |

The squad shares one job through the 8-byte team memory: each member writes its claim into its own 10-bit slot. The member in the lowest available slot coordinates. Workers never take over squad decisions, and the squad never falls back to worker logic. See [docs/design-notes.md](docs/design-notes.md) for the decision rules.

## Code conventions

- **Formatting:** `bots/test-bot/.clang-format` (LLVM base, 4-space indent, 110 columns). Format before committing:
  `clang-format -i $(find bots/test-bot/src -name '*.cpp' -o -name '*.h')`.
  The `cambc` toolchain ships clang-format in `%LOCALAPPDATA%/cambc/wasi-sdk-33/bin`.
- **Naming:** `snake_case` for functions and variables, `PascalCase` for types. `Bot` methods that belong to the worker controller start with `worker_`; squad methods start with `squad_` or live in `squad/`.
- **Files:** each `.cpp` file opens with a one-line comment saying what it owns. A new controller or composition gets its own folder or file, not a branch inside an existing function.
- **Tunables:** every heuristic coefficient goes in `policy::` in `strategy.h`, with a comment giving the value that disables it. Engine rules come from `bc::constants`; rules the SDK doesn't expose go in `rules::`.
- **Fuel:** check `bc::fuel_remaining()` against `policy::*_reserve` before any loop that could be long. Long searches keep a cursor and resume next turn rather than restarting.
- **Diagnostics:** guard logging with `if constexpr (policy::diagnostic)`. Log lines use an UPPER_CASE tag followed by `key=value` pairs (`SQUAD_JOB r=... camp=...`).
- **Comments:** explain *why* a rule exists, especially the game mechanic or failure it guards against, not what the next line does.
