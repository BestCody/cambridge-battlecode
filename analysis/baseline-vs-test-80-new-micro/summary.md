# Baseline vs test-bot: 80 games after the micro update

Ran `python scripts/evaluate.py --bot baseline --baseline test-bot --maps 00ambush,00collision,00contest,00corners,00eight,00lost-world,00swirl,01big-world --jobs 4 --out analysis/baseline-vs-test-80-new-micro` with cambc 4.0.0. The runner used both seats on each map at fuel budgets 92,000, 96,000, 100,000, 104,000, and 108,000.

| Metric | Baseline | Test-bot |
| --- | ---: | ---: |
| Wins | 61 | 19 |
| Submitted gems | 71,480 | 38,399 |
| Bot faults | 0 | 0 |

Baseline won 33 of 40 games in seat A and 28 of 40 in seat B.

| Map | Baseline wins / 10 |
| --- | ---: |
| 00ambush | 6 |
| 00collision | 5 |
| 00contest | 10 |
| 00corners | 10 |
| 00eight | 10 |
| 00lost-world | 8 |
| 00swirl | 7 |
| 01big-world | 5 |

All 80 match statistics, 80 replays, and the runner's aggregated `games.json` are in this directory. This measures the current working-tree version of `test-bot`, including its newly ported micro.
